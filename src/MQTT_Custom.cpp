#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "Globals.h"
#include "Constants.h"
#include "Log.h"

// Cliente MQTT
WiFiClient mqttEspClient;
PubSubClient mqttClient(mqttEspClient);

// Variables de tiempo para reconexión (con backoff exponencial)
unsigned long lastMqttReconnectAttempt = 0;
uint32_t mqttReconnectBackoff = quantix::constants::MQTT_RECONNECT_BASE_MS;

// Identidad Única
char uid[13];

// Tópicos Dinámicos
char topicStatus[64], topicTarget[64], topicConfig[64], topicCmd[64];
char topicTest[64], topicCal[64], topicRelays[64];

void obtenerUID()
{
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uid, sizeof(uid), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    LOG_I("mqtt", "UID=%s", uid);
}

namespace {

// Valida y extrae un `id` del documento (rango [0, SensorCount)).
bool extractSensorId(const JsonDocument& doc, const char* key, uint8_t& outId)
{
    if (!doc.containsKey(key)) return false;
    int raw = doc[key].as<int>();
    if (raw < 0 || raw >= (int)MDL.SensorCount) return false;
    outId = (uint8_t)raw;
    return true;
}

void handleConfigMsg(JsonDocument& doc)
{
    if (!doc.containsKey("configs")) return;
    JsonArray configs = doc["configs"];
    for (JsonObject c : configs)
    {
        int raw = c["idx"] | -1;
        if (raw < 0 || raw >= (int)MDL.SensorCount) {
            LOG_W("mqtt", "config: idx fuera de rango (%d)", raw);
            continue;
        }
        uint8_t id = (uint8_t)raw;

        JsonObject pid = c["config_pid"];
        if (!pid.isNull())
        {
            Sensor[id].Kp = pid["kp"] | Sensor[id].Kp;
            Sensor[id].Ki = pid["ki"] | Sensor[id].Ki;
            Sensor[id].Kd = pid["kd"] | Sensor[id].Kd;
        }
        JsonObject cal = c["calibracion"];
        if (!cal.isNull())
        {
            int pwmMin = cal["pwm_min"] | 40;
            int pwmMax = cal["pwm_max"] | (int)quantix::constants::PWM_MAX;
            pwmMin = constrain(pwmMin, 0, (int)quantix::constants::PWM_MAX);
            pwmMax = constrain(pwmMax, pwmMin, (int)quantix::constants::PWM_MAX);
            Sensor[id].MinPWM = (uint16_t)pwmMin;
            Sensor[id].MaxPWM = (uint16_t)pwmMax;
        }
        // El backend envía la clave histórica "meter_cal" (gramos/pulso).
        Sensor[id].GramosPorPulso = c["meter_cal"] | Sensor[id].GramosPorPulso;
        ResetPIDState(id);
    }
    SaveData();
}

void handleTargetMsg(JsonDocument& doc)
{
    uint8_t id;
    if (!extractSensorId(doc, "id", id)) {
        LOG_W("mqtt", "target: id inválido");
        return;
    }
    float pps = doc["pps"] | 0.0f;
    bool seccionOn = doc["seccion_on"] | false;

    if (pps < 0.0f) {
        LOG_W("mqtt", "target: pps negativo rechazado (%.2f)", pps);
        return;
    }

    Sensor[id].TargetUPM = seccionOn ? pps : 0.0f;
    Sensor[id].FlowEnabled = seccionOn;
    Sensor[id].CommTime = millis();
}

void handleTestMsg(JsonDocument& doc)
{
    uint8_t id;
    if (!extractSensorId(doc, "id", id)) {
        LOG_W("mqtt", "test: id inválido");
        return;
    }
    const char* cmd = doc["cmd"] | "stop";
    int pwmVal = doc["pwm"] | 0;

    if (pwmVal < 0 || pwmVal > (int)quantix::constants::PWM_MAX) {
        LOG_W("mqtt", "test: pwm fuera de rango (%d)", pwmVal);
        return;
    }

    if (strcmp(cmd, "start") == 0)
    {
        Sensor[id].AutoOn = false;
        Sensor[id].ManualAdjust = pwmVal;
        SetPWM(id, pwmVal);
    }
    else
    {
        Sensor[id].AutoOn = true;
        SetPWM(id, 0);
    }
    Sensor[id].CommTime = millis();
}

void handleCalMsg(JsonDocument& doc)
{
    uint8_t id;
    if (!extractSensorId(doc, "id", id)) {
        LOG_W("mqtt", "cal: id inválido");
        return;
    }
    long pulsosMeta = doc["pulsos"] | 0;
    int pwmVal = doc["pwm"] | 1000;
    const char* cmd = doc["cmd"] | "stop";

    if (pwmVal < 0 || pwmVal > (int)quantix::constants::PWM_MAX) {
        LOG_W("mqtt", "cal: pwm fuera de rango (%d)", pwmVal);
        return;
    }

    if (strcmp(cmd, "start") == 0 && pulsosMeta > 0)
    {
        Sensor[id].CalibrandoAhora = false;

        noInterrupts();
        Sensor[id].TotalPulses = 0;
        interrupts();

        Sensor[id].PulsosMetaCalibracion = (uint32_t)pulsosMeta;
        Sensor[id].ManualAdjust = pwmVal;
        Sensor[id].AutoOn = false;

        SetPWM(id, pwmVal);
        Sensor[id].CalibrandoAhora = true;

        LOG_I("cal", "Start M%u meta=%ld pwm=%d", (unsigned)id, pulsosMeta, pwmVal);
    }
    else
    {
        Sensor[id].CalibrandoAhora = false;
        Sensor[id].ManualAdjust = 0;
        Sensor[id].AutoOn = true;
        SetPWM(id, 0);
    }
    Sensor[id].CommTime = millis();
}

// --- E. PROCESAR RELAYS (corte por secciones o surco a surco) ---
//
// Formatos aceptados (el handler soporta ambos a la vez):
//  1) Máscara completa (corte masivo por secciones):
//        {"mask": 65535}          -> todas las 16 secciones ON
//        {"mask": "0xFF00"}       -> mitad alta ON, mitad baja OFF
//        {"low": 255, "high": 0}  -> idéntico con dos bytes separados
//     Cada bit representa una sección: bit 0..7 = relés 0..7 (RelayLo),
//     bit 8..15 = relés 8..15 (RelayHi).
//
//  2) Relay individual (surco a surco):
//        {"id": 3, "on": true}
//        {"id": 11, "on": false}
//     id ∈ [0, 15]. Flips solo ese bit sin tocar los demás.
//
// El handler también refresca WifiSwitchesTimer para que el timeout de
// CheckRelays no corte la salida por "sin comunicación".
void handleRelaysMsg(JsonDocument& doc)
{
    // Formato 2: id + on
    if (doc.containsKey("id") && doc.containsKey("on")) {
        int rid = doc["id"].as<int>();
        bool on = doc["on"].as<bool>();
        if (rid < 0 || rid > 15) {
            LOG_W("mqtt", "relays: id fuera de rango (%d)", rid);
            return;
        }
        if (rid < 8) {
            if (on) RelayLo |= (1u << rid);
            else    RelayLo &= ~(1u << rid);
        } else {
            uint8_t off = rid - 8;
            if (on) RelayHi |= (1u << off);
            else    RelayHi &= ~(1u << off);
        }
        WifiSwitchesTimer = millis();
        WifiMasterOn = false;  // Modo MQTT: las coms mantienen los relés vía CommTime
        for (uint8_t i = 0; i < MDL.SensorCount; ++i)
            Sensor[i].CommTime = millis();
        LOG_D("relays", "Toggle rid=%d on=%d mask=%02X%02X",
              rid, (int)on, RelayHi, RelayLo);
        return;
    }

    // Formato 1: máscara completa
    uint16_t mask = 0;
    bool haveMask = false;

    if (doc.containsKey("mask")) {
        // Aceptamos entero o string "0x..." / decimal.
        if (doc["mask"].is<const char*>()) {
            const char* s = doc["mask"].as<const char*>();
            mask = (uint16_t)strtoul(s, nullptr, 0);
        } else {
            mask = (uint16_t)(doc["mask"].as<uint32_t>() & 0xFFFFu);
        }
        haveMask = true;
    } else if (doc.containsKey("low") || doc.containsKey("high")) {
        uint8_t lo = (uint8_t)(doc["low"]  | 0);
        uint8_t hi = (uint8_t)(doc["high"] | 0);
        mask = ((uint16_t)hi << 8) | lo;
        haveMask = true;
    }

    if (!haveMask) {
        LOG_W("mqtt", "relays: falta 'mask' o 'id'+'on'");
        return;
    }

    RelayLo = (uint8_t)(mask & 0xFF);
    RelayHi = (uint8_t)((mask >> 8) & 0xFF);
    WifiSwitchesTimer = millis();
    WifiMasterOn = false;
    for (uint8_t i = 0; i < MDL.SensorCount; ++i)
        Sensor[i].CommTime = millis();

    LOG_D("relays", "Mask update %02X%02X", RelayHi, RelayLo);
}

}  // namespace

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    StaticJsonDocument<quantix::constants::JSON_CONFIG_DOC_SIZE> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
        LOG_W("mqtt", "JSON inválido: %s", error.c_str());
        return;
    }

    if (strcmp(topic, topicConfig) == 0)       handleConfigMsg(doc);
    else if (strcmp(topic, topicTarget) == 0)  handleTargetMsg(doc);
    else if (strcmp(topic, topicTest) == 0)    handleTestMsg(doc);
    else if (strcmp(topic, topicCal) == 0)     handleCalMsg(doc);
    else if (strcmp(topic, topicRelays) == 0)  handleRelaysMsg(doc);
    else LOG_D("mqtt", "Topic ignorado: %s", topic);
}
void initMQTT()
{
    obtenerUID();

    // Tópicos basados en el UID físico.
    snprintf(topicStatus, sizeof(topicStatus), "agp/quantix/%s/status_live", uid);
    snprintf(topicTarget, sizeof(topicTarget), "agp/quantix/%s/target", uid);
    snprintf(topicConfig, sizeof(topicConfig), "agp/quantix/%s/config", uid);
    snprintf(topicCmd,    sizeof(topicCmd),    "agp/quantix/%s/cmd",    uid);
    snprintf(topicTest,   sizeof(topicTest),   "agp/quantix/%s/test",   uid);
    snprintf(topicCal,    sizeof(topicCal),    "agp/quantix/%s/cal",    uid);
    snprintf(topicRelays, sizeof(topicRelays), "agp/quantix/%s/relays", uid);

    mqttClient.setBufferSize(2048);
    mqttClient.setKeepAlive(MDLnetwork.MqttKeepAlive);

    // Preferimos IP numérica si el host parsea como tal; si no, pasamos el
    // hostname a setServer para que PubSubClient haga resolución DNS.
    IPAddress ip;
    if (ip.fromString(MDLnetwork.MqttHost))
    {
        mqttClient.setServer(ip, MDLnetwork.MqttPort);
        LOG_I("mqtt", "Broker %s:%u (IP)", MDLnetwork.MqttHost, MDLnetwork.MqttPort);
    }
    else if (strlen(MDLnetwork.MqttHost) > 0)
    {
        mqttClient.setServer(MDLnetwork.MqttHost, MDLnetwork.MqttPort);
        LOG_I("mqtt", "Broker %s:%u (hostname)", MDLnetwork.MqttHost, MDLnetwork.MqttPort);
    }
    else
    {
        LOG_W("mqtt", "Host MQTT no configurado, MQTT deshabilitado");
    }
    mqttClient.setCallback(mqttCallback);
}

boolean mqttReconnect()
{
    bool ok;
    if (strlen(MDLnetwork.MqttUser) > 0)
        ok = mqttClient.connect(uid, MDLnetwork.MqttUser, MDLnetwork.MqttPass);
    else
        ok = mqttClient.connect(uid);

    if (!ok)
    {
        LOG_W("mqtt", "Reconexión falló, state=%d", mqttClient.state());
        return false;
    }

    LOG_I("mqtt", "Conectado");

    mqttClient.subscribe(topicConfig);
    mqttClient.subscribe(topicTarget);
    mqttClient.subscribe(topicCmd);
    mqttClient.subscribe(topicTest);
    mqttClient.subscribe(topicCal);
    mqttClient.subscribe(topicRelays);

    // Announce: IP actual para que el gateway lo registre.
    StaticJsonDocument<quantix::constants::JSON_ANNOUNCE_DOC_SIZE> ann;
    ann["uid"] = uid;
    ann["ip"] = WiFi.localIP().toString();
    ann["type"] = "MOTOR";

    char buffer[quantix::constants::JSON_ANNOUNCE_DOC_SIZE];
    size_t len = serializeJson(ann, buffer, sizeof(buffer));
    mqttClient.publish("agp/quantix/announcement", buffer, len);

    return true;
}

void mqttLoop()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    // Sin host configurado no intentamos reconectar (evita spam de errores DNS).
    if (strlen(MDLnetwork.MqttHost) == 0)
        return;

    if (!mqttClient.connected())
    {
        unsigned long now = millis();
        if (now - lastMqttReconnectAttempt > mqttReconnectBackoff)
        {
            lastMqttReconnectAttempt = now;
            if (mqttReconnect())
            {
                mqttReconnectBackoff = quantix::constants::MQTT_RECONNECT_BASE_MS;
            }
            else
            {
                // Backoff exponencial con tope.
                mqttReconnectBackoff = mqttReconnectBackoff * 2;
                if (mqttReconnectBackoff > quantix::constants::MQTT_RECONNECT_CAP_MS)
                    mqttReconnectBackoff = quantix::constants::MQTT_RECONNECT_CAP_MS;
            }
        }
    }
    else
    {
        mqttClient.loop();
    }
}

void sendMQTTStatus(byte ID)
{
    if (!mqttClient.connected())
        return;
    if (ID >= MDL.SensorCount)
        return;

    StaticJsonDocument<quantix::constants::JSON_STATUS_DOC_SIZE> doc;

    doc["uid"] = uid;
    doc["id"] = ID;
    doc["id_placa"] = MDL.ID;

    // Datos de tiempo real (snapshot atómico de campos volatile)
    noInterrupts();
    uint32_t totalPulsesSnapshot = Sensor[ID].TotalPulses;
    bool calibandoSnapshot = Sensor[ID].CalibrandoAhora;
    interrupts();

    doc["rpm"] = (int)Sensor[ID].RPM;
    doc["pulsos"] = (uint32_t)totalPulsesSnapshot;
    doc["pwm"] = (int)Sensor[ID].PWM;

    // --- DATOS CLAVE PARA DOSIS REAL ---
    doc["pps_real"] = Sensor[ID].Hz;          // Frecuencia actual del encoder
    doc["pps_target"] = Sensor[ID].TargetUPM; // Frecuencia buscada por el PID
    // Clave histórica "meter_cal" mantenida por compatibilidad con el backend.
    doc["meter_cal"] = Sensor[ID].GramosPorPulso;
    // ------------------------------------

    if (calibandoSnapshot)
        doc["calibrando"] = true;

    float load = (Sensor[ID].PWM / (float)quantix::constants::PWM_MAX) * 100.0f;
    if (load > 100) load = 100;
    if (load < 0)   load = 0;
    doc["load_pct"] = (int)load;

    // Estado actual de relays (corte por secciones) para que el backend
    // pueda mostrarlo y detectar divergencias. 16 bits packed.
    uint16_t relayMask = ((uint16_t)RelayHi << 8) | (uint16_t)RelayLo;
    doc["relays_mask"] = relayMask;

    char buffer[quantix::constants::JSON_STATUS_BUF_SIZE];
    size_t len = serializeJson(doc, buffer, sizeof(buffer));
    mqttClient.publish(topicStatus, buffer, len);
}