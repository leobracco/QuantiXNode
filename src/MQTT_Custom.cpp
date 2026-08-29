#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "Globals.h"
#include "OTA.h"
#include "AgpEnvelope.h"  // Tanda 2: envelope cmd_id+ttl+ack
#include "AgpSafeMode.h"  // Tanda 2: safe-mode persistido

// Cliente MQTT
WiFiClient mqttEspClient;
PubSubClient mqttClient(mqttEspClient);

// Variables de tiempo y estado
unsigned long lastMqttReconnectAttempt = 0;
const long mqttReconnectInterval = 5000;
char uid[13];

// Re-anuncio periódico — garantiza presencia aunque el broker (MQTTnet en AgIO)
// se haya reiniciado y perdido los retained, o un listener (FormNodos / OrbitX)
// arranque después del nodo. Sin esto, si la PC reinicia AgIO con el nodo ya
// conectado, el nodo queda "fantasma" hasta su próximo reboot.
unsigned long lastAnnouncementMs = 0;
const unsigned long ANNOUNCEMENT_INTERVAL_MS = 10000; // 10s

// Tanda 2 Punto 7: contadores monotónicos por tipo de payload. El consumidor
// (PC Hub) detecta gaps por pérdidas (QoS0) o reboot del nodo (cuando seq
// vuelve a 0). Wrap-around a 4.29 mil millones — a 10 Hz son ~13 años.
uint32_t agpSeqAnnouncement = 0;
uint32_t agpSeqStatusLive   = 0;

// AP watchdog — el core Arduino-ESP32 a veces tira el softAP cuando STA hace
// reconnect storms. Lo re-asentamos cada 5s.
unsigned long lastApWatchdogMs = 0;
const unsigned long AP_WATCHDOG_INTERVAL_MS = 5000;
// Throttle separado para el RE-LEVANTAMIENTO. Chequeo cada 5s; si está caído,
// re-llamo softAP() como máximo cada 30s. Sin esto, durante un STA_DISCONNECTED
// storm reintentar cada 5s puede deadlockear el stack WiFi y disparar el WDT.
unsigned long lastApRestartMs = 0;
const unsigned long AP_RESTART_THROTTLE_MS = 30000;
volatile bool apRestartInFlight = false;

extern void wdtSuspend();
extern void wdtResume();

// Tópicos (Basados en la arquitectura de Agro Parallel)
// Patrón canónico de 4 partes: agp/{producto}/{uid}/{verbo}
// NodoRegistryService.cs descarta cualquier topic con < 4 partes.
char topicTarget[64];
char topicStatus[64];
char topicConfig[64];
char topicAnnounce[64];
char topicCmdWild[64];        // agp/flow/<uid>/cmd/+   (suscripción)
char topicCmdPrefix[64];      // agp/flow/<uid>/cmd/    (para parsear verbo)
char topicCalibResult[64];    // agp/flow/<uid>/calibrar_result
char topicAutoTuneResult[64]; // agp/flow/<uid>/autotune_result
char topicCharResult[64];     // agp/flow/<uid>/caracterizar_result
char topicOtaResultado[64];   // agp/flow/<uid>/ota/resultado

// OTA: encolada desde el callback MQTT, ejecutada en loop() (handleOTACommand
// es bloqueante y reinicia el ESP — no puede correr dentro del callback).
volatile bool otaPendiente = false;
String otaUrlPendiente = "";
String otaVersionPendiente = "";
String otaSha256Pendiente = "";

// SaveConfig diferido — la escritura a LittleFS bloquea 100-500ms y NO puede
// ocurrir dentro del callback MQTT (mientras está bloqueada PubSubClient no
// procesa keepalive y otros mensajes encolados quedan esperando → WDT reset).
// El callback levanta el flag, loop() llama processPendingSave().
volatile bool mqttPendingSave = false;

void obtenerUID()
{
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uid, sizeof(uid), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    Serial.printf("🆔 FlowXNode UID: %s\n", uid);
}

// Helper: aplica el target L/min recibido del bridge al canal 0.
// El bridge AOG (FlowXBridge.cs) hoy maneja un solo "Producto" por nodo y
// asume que el caudalímetro principal está en Sensor[0]. Convertimos L/min →
// Hz (UPM) usando MeterCal del canal (pulsos/L):
//     Hz = L/min · (pulsos/L) / 60
static void applyTargetLmin(float lmin)
{
    CFG.dosisTarget = lmin;
    Sensor[0].CommTime = millis();

    float meterCal = (Sensor[0].MeterCal > 0.0f) ? Sensor[0].MeterCal : CFG.MeterCal;
    Sensor[0].TargetUPM = (meterCal > 0.0f) ? (lmin * meterCal / 60.0f) : 0.0f;

    // FlowEnabled habilita el lazo PID. Lo prendemos sólo si hay setpoint Y
    // alguna sección está abierta (sino la bomba bombea contra válvulas cerradas).
    bool wantOn = (lmin > 0.0f) && (seccionesBits > 0);
    if (Sensor[0].FlowEnabled != wantOn)
    {
        Sensor[0].FlowEnabled = wantOn;
        if (wantOn) ResetPIDState(0);
    }
}

// Mientras una detección de PWM está activa (búsqueda manual / caracterización /
// autotune / calibración) el operario tiene el tractor parado, por lo que el
// FlowXBridge publica target con sec=[0,0,..] y t=0. Si dejáramos que eso pise
// seccionesBits=0, RunChar() abortaría el barrido por "secciones cerradas"
// (CalibAuto.cpp) y la detección nunca terminaría. Replicamos exactamente la
// condición de control exclusivo del main loop: si alguna está activa,
// congelamos las secciones (ignoramos el sec entrante) hasta que termine.
static bool anyDetectionActive()
{
    for (int i = 0; i < MDL.SensorCount; i++)
    {
        if (IsManualActive(i) || IsCharActive(i) ||
            IsAutoTuneActive(i) || Sensor[i].CalibActive)
            return true;
    }
    return false;
}

// --- CALLBACK: PROCESAMIENTO DE MENSAJES RECIBIDOS ---
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error)
        return;

    String t = String(topic);

    // 1. RECIBIR TARGET (Caudal, Secciones y Control)
    // El FlowXBridge en AOG publica acá cada vez que cambia la velocidad,
    // las secciones activas o la dosis. Topic específico por UID.
    // Payload canónico:
    //   { "t": <L/min>, "sec":[1,0,1,...], "pwm_min":<int>,
    //     "pid":{"kp":..,"ki":..,"kd":..} }
    if (t == String(topicTarget))
    {
        // "sec" llega como array de 0/1 desde el bridge — se compone el bitmask
        // uint16_t (cable 0 = bit 0). Hasta 16 cables/secciones soportados.
        // Procesamos PRIMERO las secciones para que applyTargetLmin() vea el
        // seccionesBits actualizado al decidir FlowEnabled.
        // Durante una detección de PWM ignoramos por completo el sec entrante:
        // mantenemos seccionesBits congelado para que el barrido (RunChar) no se
        // aborte por los sec=0 que manda el bridge con el tractor parado.
        if (!anyDetectionActive() &&
            doc.containsKey("sec") && doc["sec"].is<JsonArray>())
        {
            JsonArray arr = doc["sec"].as<JsonArray>();
            uint16_t bits = 0;
            uint8_t  idx  = 0;
            for (JsonVariant v : arr)
            {
                if (idx >= 16) break;
                if (v.as<int>() != 0) bits |= (uint16_t)(1u << idx);
                idx++;
            }
            seccionesBits = bits;
            processValves(seccionesBits); // Actualizar Master y Secciones
        }

        if (doc.containsKey("t"))
            applyTargetLmin((float)doc["t"]);

        // Sincronización dinámica de parámetros de control (snake_case).
        // Aplicamos los gains tanto al CFG global como al Sensor[0] (que es lo
        // que usa el PID en realidad).
        if (doc.containsKey("pwm_min"))
        {
            CFG.pid.pwmMin = doc["pwm_min"];
            Sensor[0].MinPWM = CFG.pid.pwmMin;
        }
        if (doc.containsKey("pwm_max"))
        {
            // Techo del PID — si llega 0 / <=MinPWM, dejamos 4095 (sin clip extra).
            int mx = doc["pwm_max"];
            if (mx <= 0 || mx <= (int)Sensor[0].MinPWM) mx = 4095;
            if (mx > 4095) mx = 4095;
            Sensor[0].MaxPWM = mx;
        }
        if (doc.containsKey("pid"))
        {
            CFG.pid.kp = doc["pid"]["kp"];
            CFG.pid.ki = doc["pid"]["ki"];
            CFG.pid.kd = doc["pid"]["kd"];
            Sensor[0].Kp = CFG.pid.kp;
            Sensor[0].Ki = CFG.pid.ki;
            Sensor[0].Kd = CFG.pid.kd;
        }
        return;
    }

    // 2. RECIBIR CONFIGURACIÓN DE HARDWARE (Desde ventana Settings)
    //    Payload aceptado (todos los campos opcionales):
    //      { meterCal:50.0, is3Wire:true, invertRelay:false,
    //        sectionIs3Wire:[-1,1,0,-1,...] }
    //
    //    sectionIs3Wire — override per-sección del modo electroválvula:
    //      -1 = usar Is3Wire global (default)
    //       0 = forzar 2-cables (inversión de polaridad pinA/pinB)
    //       1 = forzar 3-cables (positivo o nada en pinA)
    //    Permite mezclar electroválvulas distintas en la misma barra.
    if (t == String(topicConfig))
    {
        if (doc.containsKey("meterCal"))
        {
            CFG.MeterCal = doc["meterCal"];
            Sensor[0].MeterCal = CFG.MeterCal;
        }
        if (doc.containsKey("is3Wire"))
            CFG.Is3Wire = doc["is3Wire"];
        if (doc.containsKey("invertRelay"))
            CFG.InvertRelay = doc["invertRelay"];
        if (doc.containsKey("invertMotor"))
            CFG.InvertMotor = doc["invertMotor"];

        if (doc.containsKey("sectionIs3Wire") && doc["sectionIs3Wire"].is<JsonArray>())
        {
            JsonArray arr = doc["sectionIs3Wire"].as<JsonArray>();
            uint8_t idx = 0;
            for (JsonVariant v : arr)
            {
                if (idx >= 10) break;
                int val = v.as<int>();
                // Saneamos: cualquier valor que no sea 0 ó 1 cae a -1 (global).
                CFG.SectionIs3Wire[idx] = (val == 0 || val == 1) ? (int8_t)val : (int8_t)-1;
                idx++;
            }
        }

        // Re-aplicamos el estado actual de las secciones para que los cambios
        // de modo (2/3 cables, invertRelay) tomen efecto inmediato sin
        // esperar al próximo target del bridge.
        processValves(seccionesBits);

        // NO llamar SaveConfig() acá — bloquea LittleFS 100-500ms dentro del
        // callback y puede tirar el cliente MQTT por timeout de keepalive.
        // Encolamos para que loop() lo procese fuera del callback.
        mqttPendingSave = true;
        Serial.println("✅ Configuración de hardware actualizada (guardado encolado).");
        return;
    }

    // 3. COMANDOS desde AOG (FlowXController.cs::SendCmd):
    //   agp/flow/<uid>/cmd/<verb>  con body JSON crudo.
    // Verbos soportados (alineados con FlowXLiveService.cs):
    //   calibrar_start { vol_l: 1.0, pwm: 2048, producto_id: 0 }
    //   calibrar_stop  { producto_id: 0 }
    //   autotune_start { setpoint_hz: 5.0, pwm_high: 4095, pwm_low: 200, producto_id: 0 }
    //   autotune_stop  { producto_id: 0 }
    if (t.startsWith(topicCmdPrefix))
    {
        String verb = t.substring(strlen(topicCmdPrefix));

        // ── Tanda 2: envelope cmd_id+ttl+ack ──────────────────────────────
        // Patrón idéntico a QuantiX/VistaX. FlowX usa verbo-en-topic (no
        // payload.cmd) pero el flujo es el mismo: parse _meta → dedup →
        // TTL → safe-mode gate → ejecutar → markSeen + publishAck.
        // El firmware legacy sin _meta sigue funcionando porque el envelope
        // es additive; en ese caso publishAck() se llama igual con cmdId=""
        // y el bridge lo ignora.
        AgpCmdInfo env;
        AgpEnvelope_parseFromJson(doc, env);

        // Dedup: si ya vimos este cmd_id en el ring de 16, NO re-ejecutamos
        // pero SÍ re-publicamos ack — el bridge puede haber perdido el
        // primer ack y necesita confirmación.
        if (env.hasEnvelope && AgpEnvelope_isDuplicate(env.cmdId))
        {
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "duplicate");
            return;
        }

        // TTL: si el cmd expiró (PC reintentó tarde, broker se atascó), no
        // ejecutamos — un OTA o calibración stale puede ser peligroso.
        if (env.hasEnvelope && AgpEnvelope_isExpired(env.tsMs, env.ttlMs))
        {
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "rejected", "expired");
            AgpEnvelope_markSeen(env.cmdId);
            return;
        }

        // Safe-mode gate: si el nodo crasheó ≥3 veces, solo permitimos
        // ping y clear_safe_mode. Bloqueamos OTA, calibración y autotune
        // (escriben PWM al motor — peligrosos con estado inestable).
        bool allowedInSafeMode = (verb == "ping" || verb == "clear_safe_mode");
        if (AgpSafeMode_isActive() && !allowedInSafeMode)
        {
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "rejected", "safe_mode_active");
            AgpEnvelope_markSeen(env.cmdId);
            return;
        }

        // ── Ejecutar verbo ───────────────────────────────────────────────
        // Cmds nuevos (Tanda 2):
        if (verb == "ping")
        {
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "pong");
            AgpEnvelope_markSeen(env.cmdId);
            return;
        }
        if (verb == "clear_safe_mode")
        {
            AgpSafeMode_clear();
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "safe_mode_cleared");
            AgpEnvelope_markSeen(env.cmdId);
            return;
        }

        // OTA: agp/flow/<uid>/cmd/ota  payload {"url":"...","version":"x.y.z"}
        // Se ejecuta fuera del callback en el loop principal (es bloqueante
        // y reinicia el ESP — no puede correr dentro del callback MQTT).
        if (verb == "ota")
        {
            const char *url = doc["url"] | "";
            const char *version = doc["version"] | "";
            if (strlen(url) == 0 || strlen(version) == 0)
            {
                StaticJsonDocument<128> err;
                err["uid"] = uid;
                err["status"] = "error";
                err["detalle"] = "parametros_faltantes";
                char buf[128];
                serializeJson(err, buf);
                mqttClient.publish(topicOtaResultado, buf);
                AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "rejected", "parametros_faltantes");
                AgpEnvelope_markSeen(env.cmdId);
                return;
            }
            const char *sha = doc["sha256"] | "";
            otaUrlPendiente = String(url);
            otaVersionPendiente = String(version);
            otaSha256Pendiente = String(sha);
            otaPendiente = true;
            Serial.printf("📌 OTA encolada: v%s ← %s (sha256: %s)\n",
                          version, url,
                          (strlen(sha) == 64 ? sha : "(none)"));
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "ota_queued");
            AgpEnvelope_markSeen(env.cmdId);
            return;
        }

        // producto_id es opcional (default 0). Soporta también "producto".
        byte ID = 0;
        if (doc.containsKey("producto_id"))
            ID = (byte)((int)doc["producto_id"]);
        else if (doc.containsKey("producto"))
            ID = (byte)((int)doc["producto"]);
        if (ID >= MaxProductCount) ID = 0;

        if (verb == "calibrar_start")
        {
            float volL = doc["vol_l"] | 1.0f;
            int pwm   = doc["pwm"]   | 2048;
            CalibStart(ID, volL, pwm);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "calib_started");
        }
        else if (verb == "calibrar_stop")
        {
            CalibStop(ID, false);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "calib_stopped");
        }
        else if (verb == "autotune_start")
        {
            float sp  = doc["setpoint_hz"] | 0.0f;
            int hi    = doc["pwm_high"]    | 4095;
            int lo    = doc["pwm_low"]     | 200;
            AutoTuneStart(ID, sp, hi, lo);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "autotune_started");
        }
        else if (verb == "autotune_stop")
        {
            AutoTuneStop(ID, true);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "autotune_stopped");
        }
        else if (verb == "caracterizar_start")
        {
            // Rampa PWM 0..4095 para detectar pwm_min real y hz_max. Toma el
            // PWM y publica la curva + métricas en caracterizar_result.
            // Requiere secciones abiertas (lo valida CharStart()).
            CharStart(ID);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "char_started");
        }
        else if (verb == "caracterizar_stop")
        {
            CharStop(ID, true);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "char_stopped");
        }
        else if (verb == "manual_pwm")
        {
            // Búsqueda manual de pwm_min: aplica un PWM con signo (-4095..+4095).
            // value>0 abre, value<0 cierra. Refresca el failsafe de comms-loss.
            float value = doc["value"] | 0.0f;
            ManualSet(ID, value);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "manual_pwm_set");
        }
        else if (verb == "manual_stop")
        {
            ManualStop(ID);
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "manual_stopped");
        }
        else if (verb == "save_pwm_min")
        {
            // Persiste el piso de PWM del sentido indicado. value = magnitud 0..4095.
            const char *dir = doc["dir"] | "";
            int value = doc["value"] | 0;
            value = constrain(value, 0, 4095);
            if (strcmp(dir, "pos") == 0)
            {
                CFG.pid.pwmMin = value;
                Sensor[ID].MinPWM = value;
                SaveConfig();
                AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "pwm_min_saved");
            }
            else if (strcmp(dir, "neg") == 0)
            {
                CFG.pid.pwmMinNeg = value;
                Sensor[ID].MinPWMNeg = value;
                SaveConfig();
                AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "ok", "pwm_min_saved");
            }
            else
            {
                AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "rejected", "dir_invalido");
            }
        }
        else
        {
            Serial.printf("[CMD] verbo desconocido: %s\n", verb.c_str());
            AgpEnvelope_publishAck(mqttClient, "flow", uid, env.cmdId, "rejected", "unknown_verb");
        }
        AgpEnvelope_markSeen(env.cmdId);
        return;
    }
}

void initMQTT()
{
    obtenerUID();

    // Configuración de tópicos — patrón canónico AgroParallel de 4 partes.
    // NodoRegistryService suscribe a agp/+/+/announcement y agp/+/+/status_live,
    // y descarta cualquier topic con menos de 4 partes.
    sprintf(topicTarget,        "agp/flow/%s/target",            uid);
    sprintf(topicStatus,        "agp/flow/%s/status_live",       uid);
    sprintf(topicConfig,        "agp/flow/%s/config",            uid);
    sprintf(topicAnnounce,      "agp/flow/%s/announcement",      uid);
    sprintf(topicCmdWild,       "agp/flow/%s/cmd/+",             uid);
    sprintf(topicCmdPrefix,     "agp/flow/%s/cmd/",              uid);
    sprintf(topicCalibResult,   "agp/flow/%s/calibrar_result",   uid);
    sprintf(topicAutoTuneResult,"agp/flow/%s/autotune_result",   uid);
    sprintf(topicCharResult,    "agp/flow/%s/caracterizar_result",uid);
    sprintf(topicOtaResultado,  "agp/flow/%s/ota/resultado",     uid);

    // Callback de publicación para OTA.cpp (no incluye PubSubClient directo).
    setOTAStatusCallback([](const char *status, const char *detalle, const String &version)
    {
        if (!mqttClient.connected()) return;
        StaticJsonDocument<256> doc;
        doc["uid"] = uid;
        doc["status"] = status;
        doc["version"] = version;
        if (detalle && detalle[0]) doc["detalle"] = detalle;
        char buf[256];
        size_t n = serializeJson(doc, buf);
        mqttClient.publish(topicOtaResultado, (uint8_t *)buf, n, false);
        // Pequeño loop() para que el publish realmente salga antes de bloquear/reiniciar.
        for (int i = 0; i < 5; i++) { mqttClient.loop(); delay(20); }
    });

    // Broker MQTT embebido en AgIO (MQTTnet) corriendo en la PC del tractor.
    // Host/puerto vienen de MDLnetwork (persistido en /network.json y editable
    // desde el portal web del nodo). Default 192.168.5.10:1883.
    const char *host = (MDLnetwork.BrokerHost[0] != 0)
                       ? MDLnetwork.BrokerHost : "192.168.5.10";
    uint16_t port = MDLnetwork.BrokerPort > 0 ? MDLnetwork.BrokerPort : 1883;
    mqttClient.setServer(host, port);
    // 2048 (antes 1024) — el target con sec[16] + pid{kp,ki,kd} + envelope
    // _meta (cmd_id, ttl, source, ts_ms) puede acercarse a 500B; el config
    // completo (calibración + PID por sensor × 2) lo superaba. PubSubClient
    // descarta SILENCIOSAMENTE cualquier paquete > buffer — pasaba inadvertido
    // y se reportaba como "el firmware no acepta el comando". 2048 alinea con
    // QuantiX y deja margen para configs futuros.
    mqttClient.setBufferSize(2048);
    mqttClient.setCallback(mqttCallback);

    // Keepalive corto + socket timeout MUY corto → si el broker se cae o se
    // cierra a la fuerza, PubSubClient detecta la conexión muerta rápido y
    // dispara mqttReconnect() rápido. socketTimeout=2 importa porque ES el
    // bloqueo MAX de client.connect() durante reintentos: a 8 km/h cada
    // segundo bloqueado = 2.2 m de telemetría perdida. 5s = 11 m por intento;
    // 2s = 4.4 m. El trade-off es que conexiones lentas (WiFi débil al borde
    // del lote) pueden tirar el connect antes — el throttle de 5s reintenta.
    mqttClient.setKeepAlive(15);
    mqttClient.setSocketTimeout(2);

    Serial.printf("📡 MQTT broker: %s:%u\n", host, port);
}

// Publica el announcement del nodo. Se llama:
//   · al reconectar (mqttReconnect, retained=true)
//   · cada 10s desde mqttLoop() (retained=true también — recupera presencia
//     si el broker se reinició)
//
// Publica DOS veces:
//   1) topic per-UID retained → presencia persistente (FormNodos suscribe wildcard)
//   2) topic compartido NO retained → notificación en vivo para listeners legacy
void publicarAnnouncement(bool retained)
{
    if (!mqttClient.connected()) return;

    extern const char* bootReasonStr();

    StaticJsonDocument<448> ann;
    // Tanda 2: schema + seq additive — el consumidor detecta gaps/reboots.
    ann["schema"] = "agp.flow.announcement/1";
    ann["seq"]    = ++agpSeqAnnouncement;
    ann["uid"]     = uid;
    ann["ip"]      = WiFi.localIP().toString();
    ann["ap_ip"]   = WiFi.softAPIP().toString();
    ann["type"]    = "FlowX";
    ann["device"]  = "flow";
    ann["fw"]      = FW_VERSION;
    ann["hw"]      = "SK21";
    ann["rssi"]    = WiFi.RSSI();
    ann["uptime"]  = millis() / 1000;
    ann["wifi"]    = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("");
    // boot_reason: poweron / task_wdt / brownout / panic — diagnóstico
    // de cuelgues en campo desde el panel sin USB serial.
    ann["boot_reason"] = bootReasonStr();
    // Tanda 2: safe-mode persistido tras 3 crashes seguidos.
    ann["safe_mode"]   = AgpSafeMode_isActive();
    ann["crash_count"] = AgpSafeMode_crashCount();

    char buffer[448];
    size_t n = serializeJson(ann, buffer);

    mqttClient.publish(topicAnnounce, (uint8_t *)buffer, n, retained);

    // Legacy no-retained al topic compartido para listeners viejos.
    mqttClient.publish("agp/flow/announcement", (uint8_t *)buffer, n, false);
}

boolean mqttReconnect()
{
    // Last Will Testament — el broker publica este payload retained en
    // agp/flow/{uid}/lwt si detecta caída TCP sin disconnect limpio
    // (cuelgue HW, WiFi tirado, brownout). PilotX marca offline en
    // ~keepAlive=15s, no esperando los 30s del MarkStale.
    char lwtTopic[64];
    snprintf(lwtTopic, sizeof(lwtTopic), "agp/flow/%s/lwt", uid);
    const char* lwtPayload = "{\"online\":false,\"reason\":\"unexpected_disconnect\"}";

    // connect(clientId, willTopic, willQos=1, willRetain=true, willMessage)
    if (mqttClient.connect(uid, lwtTopic, 1, true, lwtPayload))
    {
        Serial.println(F("✅ MQTT FlowXNode Conectado"));

        // Sobrescribir el LWT retained con "online" inmediatamente.
        // Sin esto, suscriptores que llegan tarde ven el último offline.
        const char* onlinePayload = "{\"online\":true}";
        mqttClient.publish(lwtTopic, (uint8_t*)onlinePayload, strlen(onlinePayload), true);

        // Suscribirse al target específico por UID. El FlowXBridge en AOG
        // publica siempre a agp/flow/{uid}/target — no hay topic global.
        mqttClient.subscribe(topicTarget);
        mqttClient.subscribe(topicConfig);
        // Comandos arbitrarios: agp/flow/<uid>/cmd/{calibrar_start,calibrar_stop,
        // autotune_start, autotune_stop, ...} — el verbo se parsea en el callback.
        mqttClient.subscribe(topicCmdWild);

        // ANUNCIO inicial — retained per-UID + legacy no-retained.
        // El periódico cada 10s lo dispara mqttLoop().
        publicarAnnouncement(true);
        lastAnnouncementMs = millis();

        return true;
    }
    return false;
}

// AP watchdog — corre desde mqttLoop() cada AP_WATCHDOG_INTERVAL_MS.
// Asegura que el softAP siga vivo aunque el core Arduino-ESP32 lo haya
// tirado durante una reconexión STA agresiva.
static void apWatchdog()
{
    unsigned long now = millis();
    if (now - lastApWatchdogMs < AP_WATCHDOG_INTERVAL_MS) return;
    lastApWatchdogMs = now;

    if (apRestartInFlight) return;

    wifi_mode_t modo = WiFi.getMode();
    IPAddress apIp = WiFi.softAPIP();
    bool apCaido = (apIp == IPAddress((uint32_t)0)) ||
                   (modo != WIFI_MODE_APSTA && modo != WIFI_MODE_AP);

    if (!apCaido) return;

    // Throttle 30s para el re-levantamiento.
    if (now - lastApRestartMs < AP_RESTART_THROTTLE_MS) return;
    lastApRestartMs = now;

    apRestartInFlight = true;
    Serial.printf("⚠ AP watchdog: modo=%d apIP=%s → re-levantando AP\n",
                  (int)modo, apIp.toString().c_str());

    // Sin delay() — el driver acepta softAP() inmediato. La IP la
    // verificamos en el próximo ciclo (5s). Los delay(50)/delay(100)
    // anteriores bloqueaban el loop PID 150ms en cada chequeo.
    WiFi.mode(WIFI_AP_STA);
    if (strlen(MDL.APpassword) < 8) strcpy(MDL.APpassword, "12345678");
    char apName[32];
    snprintf(apName, sizeof(apName), "FX-%s", uid + 3);
    // Mismo orden que en Begin.cpp: softAP() PRIMERO, softAPConfig() DESPUÉS.
    // Si se invierte, el SSID no se broadcastea aunque softAPIP() devuelva
    // 192.168.4.1 (AP "fantasma": existe internamente pero no es visible).
    bool ok = WiFi.softAP(apName, MDL.APpassword, 1, 0, 4);
    IPAddress apIP(192, 168, 4, 1);
    IPAddress apGW(192, 168, 4, 1);
    IPAddress apMask(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apGW, apMask);
    Serial.printf("📡 AP re-arranque pedido: %s (ok=%d) — verifico en próximo ciclo\n",
                  apName, ok);
    apRestartInFlight = false;
}

void mqttLoop()
{
    // AP permanente (WIFI_AP_STA desde Begin.cpp) ya está arriba siempre, así
    // que aquí solo gestionamos: (a) AP watchdog, (b) STA reconnect transparente
    // del stack del ESP32, (c) MQTT reconnect, (d) re-anuncio periódico.
    apWatchdog();

    if (WiFi.status() != WL_CONNECTED)
    {
        // Sin STA no hay broker LAN — esperar a que vuelva. El stack del ESP32
        // ya reintenta solo; no tocamos modos WiFi acá — el AP debe seguir online.
        return;
    }

    if (!mqttClient.connected())
    {
        // MQTT caído (broker reiniciado, microcorte, half-open detectado por
        // keepalive). Reintentar cada 5s sin bloquear el resto del loop.
        unsigned long now = millis();
        if (now - lastMqttReconnectAttempt > mqttReconnectInterval)
        {
            lastMqttReconnectAttempt = now;
            if (mqttReconnect())
                lastMqttReconnectAttempt = 0;
        }
        return;
    }

    // Conectado: procesar callbacks pendientes.
    mqttClient.loop();

    // Re-anunciar identidad cada ANNOUNCEMENT_INTERVAL_MS — "siempre estén
    // enviando quienes son". Garantiza que listeners que arrancan tarde
    // (FormNodos, OrbitX) vean los nodos sin esperar a que cada uno se reconecte.
    unsigned long now = millis();
    if (now - lastAnnouncementMs >= ANNOUNCEMENT_INTERVAL_MS)
    {
        lastAnnouncementMs = now;
        publicarAnnouncement(true);
    }
}

// Mapeo de Sensor.PidState (uint8_t) → string que entiende FlowXLiveService
// en AOG (lee con keys "pid_estado" | "pid_state" | "estado").
static const char *pidStateToString(uint8_t s)
{
    switch (s)
    {
    case 1: return "sin_pulsos";
    case 2: return "saturado";
    case 3: return "ok";
    default: return "off";
    }
}

// --- FUNCIÓN PARA ENVIAR TELEMETRÍA REAL ---
// Claves alineadas con FlowXLiveService.cs (AOG). Mantenemos compat con las
// claves viejas (lmin, pps_target, sec_on) por si quedan herramientas legacy
// escuchando.
//
// IMPORTANTE: FlowXBridge en AOG hoy gestiona 1 producto por nodo y usa el
// uid como clave única en el live service; si publicáramos status_live para
// los 2 canales, el segundo pisaría al primero. Por eso este firmware envía
// status_live SÓLO para el canal 0. Si en el futuro AOG soporta multi-producto,
// migrar a sub-topics tipo agp/flow/<uid>/<producto_id>/status_live.
void sendMQTTStatus(byte ID)
{
    if (!mqttClient.connected())
        return;
    if (ID != 0) return; // ver nota arriba

    StaticJsonDocument<448> doc;

    // Tanda 2: schema + seq. seq es global del nodo (no por producto_id).
    doc["schema"] = "agp.flow.status_live/1";
    doc["seq"]    = ++agpSeqStatusLive;
    doc["uid"] = uid;
    doc["id"] = ID;
    doc["producto_id"] = ID;
    doc["id_placa"] = MDL.ID;

    // UPM/TargetUPM están en Hz (pulsos/s). AOG espera L/min en
    // caudal_lmin/target_lmin (ver FxNodoLiveDto), así que convertimos acá
    // usando MeterCal (pulsos/L):  Lmin = Hz * 60 / MeterCal.
    float caudalHz = Sensor[ID].UPM;
    float targetHz = Sensor[ID].TargetUPM;
    float meterCal = (Sensor[ID].MeterCal > 0.001f) ? Sensor[ID].MeterCal : 1.0f;
    float caudalLmin = caudalHz * 60.0f / meterCal;
    float targetLmin = targetHz * 60.0f / meterCal;

    // Claves CANÓNICAS (las que lee AOG):
    doc["caudal_lmin"] = caudalLmin;
    doc["target_lmin"] = targetLmin;
    doc["error_lmin"]  = targetLmin - caudalLmin;
    doc["pwm"]         = (int)Sensor[ID].PWM;
    doc["pid_estado"]  = pidStateToString(Sensor[ID].PidState);

    // Compatibilidad legacy:
    doc["lmin"]       = caudalLmin;
    doc["pps_target"] = targetHz; // sigue en Hz por compatibilidad histórica
    doc["sec_on"]     = Sensor[ID].FlowEnabled;
    doc["pulsos"]     = Sensor[ID].TotalPulses;

    char buffer[448];
    size_t n = serializeJson(doc, buffer);
    mqttClient.publish(topicStatus, (const uint8_t *)buffer, n, false);
}

// ----------------------------------------------------------------------------
// Publicación de resultados de calibración y auto-tune — los consume
// FlowXLiveService.cs vía topics agp/flow/<uid>/{calibrar_result,autotune_result}.
// ----------------------------------------------------------------------------

void publishCalibResult(byte ID, bool ok, uint32_t pulsos, uint32_t durationMs, const char *err)
{
    if (!mqttClient.connected()) return;
    StaticJsonDocument<256> doc;
    doc["uid"] = uid;
    doc["producto_id"] = ID;
    doc["ok"] = ok;
    doc["pulsos"] = pulsos;
    doc["duration_ms"] = durationMs;
    doc["error"] = err ? err : "";
    char buffer[256];
    size_t n = serializeJson(doc, buffer);
    mqttClient.publish(topicCalibResult, (const uint8_t *)buffer, n, true);
}

void publishAutoTuneResult(byte ID, bool ok, float kp, float ki, float kd,
                           float ku, float tuMs, const char *err)
{
    if (!mqttClient.connected()) return;
    StaticJsonDocument<320> doc;
    doc["uid"] = uid;
    doc["producto_id"] = ID;
    doc["ok"] = ok;
    doc["kp"] = kp;
    doc["ki"] = ki;
    doc["kd"] = kd;
    doc["ku"] = ku;
    doc["tu_ms"] = tuMs;
    doc["error"] = err ? err : "";
    char buffer[320];
    size_t n = serializeJson(doc, buffer);
    mqttClient.publish(topicAutoTuneResult, (const uint8_t *)buffer, n, true);
}

// ----------------------------------------------------------------------------
// publishCharResult — resultado de caracterización (barrido PWM→Hz) en
// agp/flow/<uid>/caracterizar_result. Incluye:
//   · pwm_min        — el primer PWM donde el motor reacciona (Hz > 0).
//   · pwm_min_estable— PWM donde el caudal queda estable (umbral arbitrario).
//   · hz_max         — Hz máximo alcanzado (techo del caudalímetro o bomba).
//   · lmin_max       — equivalente en L/min (calculado con MeterCal).
//   · pwm[] / hz[]   — curva completa del barrido (la UI grafica esto).
// retained=true así la UI puede recuperar el último resultado al abrir el
// modal sin tener que reejecutar la caracterización.
// ----------------------------------------------------------------------------
void publishCharResult(byte ID, bool ok, int pwmMin, int pwmMinEstable,
                       float hzMax, float lminMax,
                       const int *pwmCurve, const float *hzCurve, int curveLen,
                       const char *err)
{
    if (!mqttClient.connected()) return;

    // Dimensionar el doc para hasta ~30 puntos: header + 2 arrays floats/ints.
    // ~512 bytes por header y ~40 bytes por punto (int+float JSON) → 1700 max.
    DynamicJsonDocument doc(2048);
    doc["uid"] = uid;
    doc["producto_id"] = ID;
    doc["ok"] = ok;
    doc["pwm_min"] = pwmMin;
    doc["pwm_min_estable"] = pwmMinEstable;
    doc["hz_max"] = hzMax;
    doc["lmin_max"] = lminMax;
    doc["error"] = err ? err : "";

    JsonArray pwmArr = doc.createNestedArray("pwm");
    JsonArray hzArr  = doc.createNestedArray("hz");
    if (pwmCurve && hzCurve && curveLen > 0)
    {
        for (int i = 0; i < curveLen; i++)
        {
            pwmArr.add(pwmCurve[i]);
            hzArr.add(hzCurve[i]);
        }
    }

    char buffer[2048];
    size_t n = serializeJson(doc, buffer, sizeof(buffer));
    mqttClient.publish(topicCharResult, (const uint8_t *)buffer, n, true);
}

// Procesa SaveConfig diferido — escribe a LittleFS desde loop() en vez del
// callback MQTT. Llamada desde main.cpp:loop().
void processPendingSave()
{
    if (!mqttPendingSave) return;
    mqttPendingSave = false;
    // H8: noInterrupts() solo desactivaba IRQ del CORE actual — la PulseISR
    // puede correr en el otro core mientras LittleFS escribe (50-200ms),
    // y desactivar IRQ por tanto tiempo tira el WiFi/MQTT keep-alive.
    // Además SaveConfig sólo serializa campos de Sensor[] tocados por loop()
    // (Mode/CalFactor/PulseSampleSize/etc.) — los contadores tocados por la
    // ISR (PulseTime/PulseCount/Samples[]) NO se persisten. Quitarlo es
    // estrictamente más correcto.
    SaveConfig();
    Serial.println("💾 SaveConfig diferido completado.");
}

// Ejecuta una OTA encolada por el callback MQTT. Bloqueante y reinicia el ESP.
// Llamada desde loop() en main.cpp.
void procesarOtaPendiente()
{
    if (!otaPendiente) return;
    otaPendiente = false;

    // Frenar todos los canales antes de la OTA: PWM a 0, bomba parada.
    for (int i = 0; i < MDL.SensorCount && i < MaxProductCount; i++)
    {
        Sensor[i].FlowEnabled = false;
        Sensor[i].TargetUPM = 0;
        Sensor[i].PWM = 0;
        SetPWM(i, 0);
    }
    // Cerrar TODAS las válvulas físicas (Master + secciones) antes de la OTA.
    // Sin esto, las válvulas quedan en su último estado durante 30-120s de
    // descarga + flash. Con presión residual + inercia de bomba puede seguir
    // saliendo líquido. processValves(0) cierra Master + 8 secciones (revisar
    // Relays.cpp::processValves).
    processValves(0);
    // Tras procesar el bitmask=0, también cancelar cualquier autotune/calib
    // en curso — el reboot post-OTA tira sus flags RAM pero el log de result
    // queda inconsistente si no las paramos limpio acá.
    for (int i = 0; i < MaxProductCount; i++)
    {
        if (IsAutoTuneActive(i)) AutoTuneStop(i, true);
        if (Sensor[i].CalibActive) CalibStop(i, true);
    }
    Serial.println("⛔ Salidas detenidas por OTA (PWM=0, válvulas cerradas, autotune/calib abortados)");

    // HTTPClient + Update descarga un .bin de ~1MB, puede tardar 30-120s.
    // Excede el WDT_TIMEOUT_SEC del loop principal. Desuscribimos la tarea
    // del WDT antes y la reagregamos después (en caso improbable de que
    // retorne sin reboot, p.ej. URL inválida con timeout corto).
    wdtSuspend();
    handleOTACommand(otaUrlPendiente, otaVersionPendiente, otaSha256Pendiente);
    wdtResume();

    // Si llegamos acá es porque el OTA falló sin reboot (URL inválida, SHA
    // mismatch, write error). Mantener las salidas frenadas: ya están en 0,
    // pero el próximo `target` del bridge reactivará FlowEnabled. Esto es
    // intencional — el operario decide cuándo retomar. NO re-abrir nada acá.
}