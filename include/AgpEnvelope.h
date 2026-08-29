// ============================================================================
// AgpEnvelope.h
// Sobre estándar para mensajes MQTT del ecosistema Agro Parallel (Tanda 2).
//
// Lado firmware del envelope C# (AgroParallel.Models/AgpEnvelope.cs). Mantener
// SIMÉTRICO con la PC — si cambia un nombre de campo acá, cambia allá.
//
// Filosofía:
//   · ADITIVO en payload: el envelope vive en `_meta`. Si llega un cmd
//     SIN _meta (PC vieja, retransmisión legacy) lo ejecutamos igual pero NO
//     enviamos ack (no hay cmd_id que devolver).
//   · DEDUP: ring buffer de últimos N cmd_id procesados. Si llega un cmd
//     repetido (broker reentregando QoS1, retry del bridge) lo descartamos
//     PERO igualmente publicamos ack — el bridge necesita la confirmación
//     aunque el firmware ya lo había ejecutado.
//   · TTL: rechazamos cmd con ts_ms + ttl_ms < tiempo_actual_local. Esto
//     requiere que el firmware tenga reloj razonable; en ESP32 sin RTC
//     usamos millis() pero el ts_ms del envelope es Unix ms — la comparación
//     se hace sobre un epoch_local sincronizado al primer cmd recibido.
//
// USO en MQTT_Custom.cpp:
//
//   void mqttCallback(char* topic, byte* payload, unsigned int length) {
//       ...
//       AgpCmdInfo cmd;
//       bool hasEnvelope = AgpEnvelope_parseFromJson(doc, cmd);
//       if (hasEnvelope) {
//           if (AgpEnvelope_isDuplicate(cmd.cmdId)) {
//               // Repetir ack para que el bridge pueda cerrar la promesa.
//               AgpEnvelope_publishAck(mqttClient, uid, cmd.cmdId, "ok", "dedup");
//               return;
//           }
//           if (AgpEnvelope_isExpired(cmd.tsMs, cmd.ttlMs)) {
//               AgpEnvelope_publishAck(mqttClient, uid, cmd.cmdId, "rejected", "expired");
//               return;
//           }
//       }
//       // ... ejecutar el comando real ...
//       if (hasEnvelope) AgpEnvelope_publishAck(mqttClient, uid, cmd.cmdId, "ok", "");
//   }
// ============================================================================

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

// Ring buffer de cmd_ids ya vistos. 64 entradas cubre ráfagas de calibración
// + autotune simultáneas + retries del bridge sin riesgo de re-procesar un
// cmd cuyo cmd_id ya fue desplazado. Si se llena, sobreescribe el más viejo
// — los cmds expirarán por TTL antes de llegar a ese punto. (H7)
//
// M10 — atención: este archivo es IDÉNTICO byte-a-byte en
//   QuantiX (Quantix2Motors/include/AgpEnvelope.h)
//   VistaX  (vistax-node/src/AgpEnvelope.h)
//   FlowX   (FlowXNode/include/AgpEnvelope.h)
// Cualquier cambio acá DEBE replicarse en los otros dos. Unificar a una
// PlatformIO `lib/` compartida queda pendiente.
#ifndef AGP_CMD_DEDUP_SIZE
#define AGP_CMD_DEDUP_SIZE 64
#endif

#ifndef AGP_CMD_ID_MAX_LEN
#define AGP_CMD_ID_MAX_LEN 24
#endif

struct AgpCmdInfo {
    bool hasEnvelope;
    char cmdId[AGP_CMD_ID_MAX_LEN + 1];
    long long tsMs;
    int ttlMs;
    char source[16];
    int ver;
    long seq;
};

namespace _AgpEnvImpl {
    inline char (&_dedupRing())[AGP_CMD_DEDUP_SIZE][AGP_CMD_ID_MAX_LEN + 1] {
        static char ring[AGP_CMD_DEDUP_SIZE][AGP_CMD_ID_MAX_LEN + 1] = {{0}};
        return ring;
    }
    inline uint8_t &_dedupCursor() {
        static uint8_t cur = 0;
        return cur;
    }
    // epoch_local: offset entre Unix ms (del envelope PC) y millis() (firmware).
    // Se setea con el primer cmd que llega con envelope; sirve para evaluar
    // ttl_ms con suficiente precisión (±keepalive PC↔firmware ≈ 1-5s).
    inline long long &_epochAnchorMs() {
        static long long anchor = 0; // 0 = sin anclar
        return anchor;
    }
    inline unsigned long &_anchorLocalMs() {
        static unsigned long lm = 0;
        return lm;
    }
}

/// Parsea el envelope `_meta` de un JsonDocument ya parseado. Devuelve true
/// si encontró un envelope válido (con cmd_id). Si no hay _meta, devuelve
/// false — el caller puede ejecutar el cmd legacy pero sin ack.
inline bool AgpEnvelope_parseFromJson(JsonDocument &doc, AgpCmdInfo &out) {
    out.hasEnvelope = false;
    out.cmdId[0] = 0;
    out.tsMs = 0;
    out.ttlMs = 0;
    out.source[0] = 0;
    out.ver = 0;
    out.seq = 0;

    if (!doc.containsKey("_meta")) return false;
    JsonObject meta = doc["_meta"].as<JsonObject>();
    if (meta.isNull()) return false;

    const char* cid = meta["cmd_id"] | "";
    if (!cid || !cid[0]) return false; // sin cmd_id no podemos hacer ack
    strncpy(out.cmdId, cid, AGP_CMD_ID_MAX_LEN);
    out.cmdId[AGP_CMD_ID_MAX_LEN] = 0;

    out.tsMs = (long long)(meta["ts_ms"] | 0LL);
    out.ttlMs = (int)(meta["ttl_ms"] | 0);
    const char* src = meta["source"] | "";
    strncpy(out.source, src, sizeof(out.source) - 1);
    out.source[sizeof(out.source) - 1] = 0;
    out.ver = (int)(meta["ver"] | 1);
    out.seq = (long)(meta["seq"] | 0);
    out.hasEnvelope = true;

    // Anclar epoch local con el primer ts_ms recibido. Si llegan ts_ms
    // posteriores con un drift grande contra el ancla (>60s), re-anclar:
    // probable que la PC se haya sincronizado por NTP o que el operario
    // haya cambiado la hora.
    if (out.tsMs > 0) {
        long long &anchor = _AgpEnvImpl::_epochAnchorMs();
        unsigned long &anchorLocal = _AgpEnvImpl::_anchorLocalMs();
        unsigned long nowLocal = millis();
        if (anchor == 0) {
            anchor = out.tsMs;
            anchorLocal = nowLocal;
        } else {
            // Estimado local del ts_ms actual usando ancla.
            long long estimated = anchor + (long long)(nowLocal - anchorLocal);
            long long drift = out.tsMs - estimated;
            if (drift > 60000LL || drift < -60000LL) {
                anchor = out.tsMs;
                anchorLocal = nowLocal;
            }
        }
    }
    return true;
}

/// True si el cmd_id ya fue procesado recientemente. En caso afirmativo,
/// el caller debería NO ejecutar el cmd pero SÍ publicar ack — porque el
/// bridge necesita confirmación independientemente de si era retry.
inline bool AgpEnvelope_isDuplicate(const char* cmdId) {
    if (!cmdId || !cmdId[0]) return false;
    auto &ring = _AgpEnvImpl::_dedupRing();
    for (int i = 0; i < AGP_CMD_DEDUP_SIZE; i++) {
        if (ring[i][0] && strncmp(ring[i], cmdId, AGP_CMD_ID_MAX_LEN) == 0) return true;
    }
    return false;
}

/// Marca un cmd_id como visto. Llamar DESPUÉS de ejecutar el cmd, así si
/// crasheamos durante la ejecución el bridge re-envía y reintentamos.
inline void AgpEnvelope_markSeen(const char* cmdId) {
    if (!cmdId || !cmdId[0]) return;
    auto &ring = _AgpEnvImpl::_dedupRing();
    auto &cur = _AgpEnvImpl::_dedupCursor();
    strncpy(ring[cur], cmdId, AGP_CMD_ID_MAX_LEN);
    ring[cur][AGP_CMD_ID_MAX_LEN] = 0;
    cur = (cur + 1) % AGP_CMD_DEDUP_SIZE;
}

/// True si el cmd ya venció su TTL contra el reloj local estimado.
/// Si no hay ancla todavía (primer cmd) NUNCA expira — preferimos ejecutar
/// que rechazar por falta de info.
inline bool AgpEnvelope_isExpired(long long tsMs, int ttlMs) {
    if (ttlMs <= 0) return false;
    long long anchor = _AgpEnvImpl::_epochAnchorMs();
    unsigned long anchorLocal = _AgpEnvImpl::_anchorLocalMs();
    if (anchor == 0 || tsMs <= 0) return false;
    long long nowEstimated = anchor + (long long)(millis() - anchorLocal);
    return (tsMs + (long long)ttlMs) < nowEstimated;
}

/// Publica el ack del comando. Topic: `agp/{prod}/{uid}/ack`.
/// status: "ok" | "rejected" | "error". detail: causa o "".
inline void AgpEnvelope_publishAck(
    PubSubClient &client, const char* productLo,
    const char* uid, const char* cmdId,
    const char* status, const char* detail)
{
    if (!cmdId || !cmdId[0]) return;
    StaticJsonDocument<256> doc;
    doc["cmd_id"] = cmdId;
    doc["status"] = status ? status : "ok";
    doc["detail"] = detail ? detail : "";
    doc["ts_ms"] = (uint32_t)millis(); // Unix ms ideal, pero millis basta para latencia
    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    char topic[64];
    snprintf(topic, sizeof(topic), "agp/%s/%s/ack", productLo, uid);
    client.publish(topic, (uint8_t*)buf, n, false);
}
