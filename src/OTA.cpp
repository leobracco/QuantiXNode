// ============================================================================
// OTA.cpp - Firmware update via HTTP (descarga + flash + reboot)
// Patrón portado de VistaX/QuantiX. Mismo binario lógico, distinto producto.
// ============================================================================

#include "OTA.h"
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

static std::function<void(const char *, const char *, const String &)> _statusCb;

void setOTAStatusCallback(std::function<void(const char *, const char *, const String &)> cb)
{
    _statusCb = cb;
}

static void notify(const char *status, const char *detalle, const String &version)
{
    if (_statusCb) _statusCb(status, detalle, version);
}

static void sha256ToHex(const uint8_t *digest, char *out65)
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
    {
        out65[i * 2 + 0] = hexd[(digest[i] >> 4) & 0xF];
        out65[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out65[64] = '\0';
}

void handleOTACommand(const String &url, const String &version, const String &expectedSha256)
{
    Serial.println("\n=============================");
    Serial.println("🔄 OTA: Iniciando actualización");
    Serial.println("URL: " + url);
    Serial.println("Versión destino: " + version);
    if (expectedSha256.length() == 64)
        Serial.println("SHA-256 esperado: " + expectedSha256);
    else
        Serial.println("SHA-256 esperado: (no provisto — sin verificación)");
    Serial.println("=============================");

    notify("iniciando", "", version);

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    if (mbedtls_sha256_starts_ret(&shaCtx, 0) != 0)
    {
        Serial.println("❌ OTA: mbedtls_sha256_starts_ret falló");
        notify("error", "sha256_init_fallido", version);
        mbedtls_sha256_free(&shaCtx);
        return;
    }

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    // Permitir redirects (FirmwareLanServer no redirige, pero por si en el
    // futuro se sirve desde Nginx/proxy).
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        Serial.println("❌ OTA: Error HTTP " + String(httpCode));
        char det[32];
        snprintf(det, sizeof(det), "http_%d", httpCode);
        notify("error", det, version);
        http.end();
        mbedtls_sha256_free(&shaCtx);
        return;
    }

    int contentLength = http.getSize();
    Serial.println("📦 Tamaño firmware: " + String(contentLength) + " bytes");

    if (contentLength <= 0)
    {
        Serial.println("❌ OTA: Content-Length inválido");
        notify("error", "content_length_invalido", version);
        http.end();
        mbedtls_sha256_free(&shaCtx);
        return;
    }

    if (!Update.begin(contentLength))
    {
        Serial.println("❌ OTA: No hay espacio en flash");
        notify("error", "sin_espacio_flash", version);
        http.end();
        mbedtls_sha256_free(&shaCtx);
        return;
    }

    // Stream con timeout manual de inactividad — Update.writeStream() del core
    // hace readBytes() bloqueante sin timeout configurable; con wdtSuspend()
    // activo un servidor caído nos deja colgados forever. Acá leemos en chunks
    // y abortamos si pasamos STREAM_TIMEOUT_MS sin recibir bytes.
    WiFiClient *stream = http.getStreamPtr();
    const size_t BUF_SIZE = 1024;
    uint8_t buf[BUF_SIZE];
    size_t written = 0;
    unsigned long lastProgressMs = millis();
    const unsigned long STREAM_TIMEOUT_MS = 60000; // 60s sin progreso → abort

    while (written < (size_t)contentLength && stream->connected())
    {
        size_t avail = stream->available();
        if (avail > 0)
        {
            size_t toRead = avail > BUF_SIZE ? BUF_SIZE : avail;
            int got = stream->readBytes(buf, toRead);
            if (got > 0)
            {
                size_t w = Update.write(buf, (size_t)got);
                if (w != (size_t)got)
                {
                    Serial.printf("❌ OTA: Update.write fallido %u/%d\n", (unsigned)w, got);
                    notify("error", "write_fallido", version);
                    http.end();
                    mbedtls_sha256_free(&shaCtx);
                    return;
                }
                (void)mbedtls_sha256_update_ret(&shaCtx, buf, (size_t)got);
                written += w;
                lastProgressMs = millis();
            }
        }
        else
        {
            if (millis() - lastProgressMs > STREAM_TIMEOUT_MS)
            {
                Serial.printf("❌ OTA: timeout sin progreso (%u/%d bytes)\n",
                              (unsigned)written, contentLength);
                notify("error", "timeout_stream", version);
                http.end();
                mbedtls_sha256_free(&shaCtx);
                return;
            }
            delay(10);
        }
    }

    if (written != (size_t)contentLength)
    {
        Serial.println("❌ OTA: Escritura incompleta (" + String(written) + "/" + String(contentLength) + ")");
        notify("error", "escritura_incompleta", version);
        http.end();
        mbedtls_sha256_free(&shaCtx);
        return;
    }

    uint8_t digest[32];
    char gotHex[65];
    if (mbedtls_sha256_finish_ret(&shaCtx, digest) != 0)
    {
        Serial.println("❌ OTA: mbedtls_sha256_finish_ret falló");
        notify("error", "sha256_finish_fallido", version);
        Update.abort();
        http.end();
        mbedtls_sha256_free(&shaCtx);
        return;
    }
    mbedtls_sha256_free(&shaCtx);
    sha256ToHex(digest, gotHex);
    Serial.println("SHA-256 calculado: " + String(gotHex));

    if (expectedSha256.length() == 64)
    {
        bool match = true;
        for (int i = 0; i < 64; i++)
        {
            char a = gotHex[i];
            char b = expectedSha256.charAt(i);
            if (b >= 'A' && b <= 'F') b = (char)(b + ('a' - 'A'));
            if (a != b) { match = false; break; }
        }
        if (!match)
        {
            Serial.println("❌ OTA: SHA-256 mismatch — abortando");
            notify("error", "sha256_mismatch", version);
            Update.abort();
            http.end();
            return;
        }
        Serial.println("✓ SHA-256 verificado");
    }

    if (!Update.end())
    {
        Serial.println("❌ OTA: Error al finalizar: " + String(Update.getError()));
        notify("error", "finalizacion_fallida", version);
        http.end();
        return;
    }

    http.end();

    Serial.println("✅ OTA: Firmware instalado");
    Serial.println("🔄 OTA: Reiniciando en 3s...");
    notify("ok", "", version);

    delay(3000); // tiempo para que MQTT publique el resultado
    ESP.restart();
}
