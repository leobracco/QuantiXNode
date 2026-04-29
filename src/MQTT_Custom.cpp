#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "Globals.h" // Importante para acceder a MDL y Sensor

// Cliente MQTT
WiFiClient mqttEspClient;
PubSubClient mqttClient(mqttEspClient);

// Variables de tiempo para reconexión
unsigned long lastMqttReconnectAttempt = 0;
const long mqttReconnectInterval = 5000;
uint8_t mqttFailCount = 0;
uint8_t wifiFailCount = 0;
bool apFallbackActive = false;

// Debug remoto: se activa desde QuantiX enviando a agp/quantix/{UID}/debug
bool mqttDebugEnabled = false;
uint32_t mqttMsgCount = 0;

// Flag para diferir SaveData() al loop principal (evita bloqueo flash en callback).
volatile bool mqttPendingSave = false;

// Identidad Única
char uid[13];

// Tópicos Dinámicos
char topicStatus[64], topicTarget[64], topicConfig[64], topicCmd[64], topicTest[64], topicCal[64], topicDebug[64];

void obtenerUID()
{
    uint64_t mac = ESP.getEfuseMac();
    // Formato: QX-XXXXXXXX (8 hex del MAC, prefijado con QX- para identificar QuantiX)
    snprintf(uid, sizeof(uid), "QX-%08X", (uint32_t)mac);
    Serial.printf("🆔 UID del Dispositivo: %s\n", uid);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    mqttMsgCount++;
    String t = String(topic);

    // Debug verbose (activable via /debug topic).
    if (mqttDebugEnabled)
    {
        char debugBuf[301];
        int copyLen = min((int)length, 300);
        memcpy(debugBuf, payload, copyLen);
        debugBuf[copyLen] = 0;
        Serial.printf("📩 %s: %s\n", topic, debugBuf);
    }

    if (t.endsWith("/debug"))
    {
        mqttDebugEnabled = (strstr((char*)payload, "true") != NULL);
        Serial.printf("🔍 DEBUG: %s\n", mqttDebugEnabled ? "ON" : "OFF");
        return;
    }

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
        Serial.print(F("❌ Error JSON: "));
        Serial.println(error.f_str());
        return;
    }

    // --- A. PROCESAR CONFIGURACIÓN ---
    if (t == String(topicConfig))
    {
        if (doc.containsKey("configs"))
        {
            JsonArray configs = doc["configs"];
            for (JsonObject c : configs)
            {
                int id = c["idx"] | 0;
                if (id < MDL.SensorCount)
                {
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
                        Sensor[id].MinPWM = cal["pwm_min"] | Sensor[id].MinPWM;
                        Sensor[id].MaxPWM = cal["pwm_max"] | Sensor[id].MaxPWM;
                    }
                    Sensor[id].MeterCal = c["meter_cal"] | Sensor[id].MeterCal;

                    // Campos nuevos PID v2
                    if (c.containsKey("motor_type"))
                        Sensor[id].MotorType = (MotorType_t)(c["motor_type"] | 0);
                    if (c.containsKey("max_hz"))
                        Sensor[id].MaxHz = c["max_hz"] | Sensor[id].MaxHz;
                    if (c.containsKey("ff_gain"))
                        Sensor[id].FFGain = c["ff_gain"] | Sensor[id].FFGain;
                    if (c.containsKey("alpha"))
                        Sensor[id].Alpha = c["alpha"] | Sensor[id].Alpha;
                    if (c.containsKey("slew_rate_per_sec"))
                        Sensor[id].SlewRatePerSec = c["slew_rate_per_sec"] | Sensor[id].SlewRatePerSec;
                    if (c.containsKey("pid_time"))
                        Sensor[id].PIDtime = c["pid_time"] | Sensor[id].PIDtime;
                    if (c.containsKey("deadband"))
                        Sensor[id].Deadband = c["deadband"] | Sensor[id].Deadband;
                    if (c.containsKey("max_integral"))
                        Sensor[id].MaxIntegral = c["max_integral"] | Sensor[id].MaxIntegral;
                    if (c.containsKey("pulses_per_rev"))
                        Sensor[id].PulsesPerRev = c["pulses_per_rev"] | Sensor[id].PulsesPerRev;
                    // Hidráulico
                    if (c.containsKey("hyd_hysteresis"))
                        Sensor[id].HydHysteresis = c["hyd_hysteresis"] | Sensor[id].HydHysteresis;
                    if (c.containsKey("hyd_pwm_freq"))
                        Sensor[id].HydPWMFreq = c["hyd_pwm_freq"] | Sensor[id].HydPWMFreq;

                    ResetPIDState(id);
                }
            }
            mqttPendingSave = true; // Diferido al loop — SaveData() bloquea flash 5-20ms
            CachePulseFilter();   // Actualizar filtro ISR si cambió PulseMin
        }
    }

    // --- B. PROCESAR TARGET (Dosis en tiempo real) ---
    else if (t == String(topicTarget))
    {
        int id = doc["id"] | 0;
        if (id < MDL.SensorCount)
        {
            float pps = doc["pps"] | 0.0f;
            bool seccionOn = doc["seccion_on"] | false;
            Sensor[id].TargetUPM = seccionOn ? pps : 0.0f;
            Sensor[id].FlowEnabled = seccionOn;
            Sensor[id].CommTime = millis();

            // CORTE INMEDIATO: si la sección se apaga, frenar motor + MOSFET.
            if (!seccionOn)
            {
                Sensor[id].PWM = 0;
                SetPWM(id, 0);
                ResetPIDState(id);
                Serial.printf("⛔ M%d CORTADO (seccion OFF)\n", id);
            }

            // Relay byte se actualiza con el mensaje /sections (abajo).
        }
    }

    // --- B2. SECCIONES INDIVIDUALES (para relays/MOSFET) ---
    // Topic: agp/quantix/{UID}/sections
    // Payload: {"lo":255,"hi":0}  ← cada bit = 1 sección
    // O payload: [1,1,0,0,1,1,0,0] ← array booleano
    else if (t.endsWith("/sections"))
    {
        if (doc.is<JsonArray>())
        {
            // Array format: [1,1,0,0,1,1,0,0]
            JsonArray arr = doc.as<JsonArray>();
            RelayLo = 0;
            RelayHi = 0;
            for (int i = 0; i < (int)arr.size() && i < 16; i++)
            {
                bool on = arr[i];
                if (i < 8)
                {
                    if (on) bitSet(RelayLo, i); else bitClear(RelayLo, i);
                }
                else
                {
                    if (on) bitSet(RelayHi, i - 8); else bitClear(RelayHi, i - 8);
                }
            }
        }
        else
        {
            // Object format: {"lo":255,"hi":0}
            RelayLo = doc["lo"] | 0;
            RelayHi = doc["hi"] | 0;
        }

        for (int i = 0; i < MDL.SensorCount; i++)
            Sensor[i].CommTime = millis();

        // SIEMPRE loguear cambios de secciones — es lo más importante.
        Serial.printf("🔌 SECTIONS: RelayLo=0x%02X [", RelayLo);
        for (int i = 0; i < 8; i++)
            Serial.printf("SA%d:%s ", i+1, bitRead(RelayLo, i) ? "ON" : "off");
        Serial.println("]");
    }

    // --- C0. AUTO-TUNE PID ---
    else if (t == String(topicCmd))
    {
        const char *cmd = doc["cmd"] | "";
        int id = doc["id"] | 0;
        if (id < MDL.SensorCount)
        {
            if (strcmp(cmd, "autotune_start") == 0)
            {
                AutoTuneStart(id);
                Serial.printf("🎯 AutoTune M%d iniciado por MQTT\n", id);
            }
            else if (strcmp(cmd, "autotune_stop") == 0)
            {
                AutoTuneStop(id);
                Serial.printf("🎯 AutoTune M%d detenido por MQTT\n", id);
            }
        }
    }

    // --- C. PROCESAR TEST (Slider web) ---
    else if (t == String(topicTest))
    {
        const char *cmd = doc["cmd"] | "stop";
        int id = doc["id"] | 0;
        int pwmVal = doc["pwm"] | 0;
        // Validar PWM: hidráulico no puede invertir
        if (Sensor[id].MotorType == MOTOR_HYDRAULIC)
            pwmVal = constrain(pwmVal, 0, 4095);
        else
            pwmVal = constrain(pwmVal, -4095, 4095);
        if (id < MDL.SensorCount)
        {
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
        }
    }

    // --- D. PROCESAR CALIBRACIÓN ---
    else if (t == String(topicCal))
    {
        int id = doc["id"] | 0;
        long pulsosMeta = doc["pulsos"] | 0;
        int pwmVal = constrain(doc["pwm"] | 1000, 0, 4095);
        const char *cmd = doc["cmd"] | "stop";

        if (id < MDL.SensorCount)
        {
            if (strcmp(cmd, "start") == 0 && pulsosMeta > 0)
            {
                Sensor[id].CalibActive = false;
                ResetPulseCounters(id); // Reset limpio de ISR + TotalPulses

                Sensor[id].CalibTargetPulses = pulsosMeta;
                Sensor[id].ManualAdjust = pwmVal; // <--- GUARDAMOS EL PWM AQUÍ
                Sensor[id].AutoOn = false;

                SetPWM(id, pwmVal);
                Sensor[id].CalibActive = true;

                Serial.printf("⚖️ START: M%d, Meta %ld, PWM %d\n", id, pulsosMeta, pwmVal);
            }
            else
            {
                Sensor[id].CalibActive = false;
                Sensor[id].ManualAdjust = 0;
                Sensor[id].AutoOn = true;
                SetPWM(id, 0);
            }
        }
    }
}
void initMQTT()
{
    obtenerUID();

    // Configuramos los tópicos basados en el UID físico
    sprintf(topicStatus, "agp/quantix/%s/status_live", uid);
    sprintf(topicTarget, "agp/quantix/%s/target", uid);
    sprintf(topicConfig, "agp/quantix/%s/config", uid);
    sprintf(topicCmd, "agp/quantix/%s/cmd", uid);
    sprintf(topicTest, "agp/quantix/%s/test", uid);
    sprintf(topicCal, "agp/quantix/%s/cal", uid);
    sprintf(topicDebug, "agp/quantix/%s/debug", uid);

    // AJUSTA LA IP AQUÍ A LA DE TU SERVIDOR/PC
    mqttClient.setBufferSize(2048);
    mqttClient.setServer(IPAddress(192, 168, 1, 11), 1883);
    mqttClient.setCallback(mqttCallback);
}

boolean mqttReconnect()
{
    if (mqttClient.connect(uid))
    {
        Serial.println(F("✅ MQTT Conectado"));

        mqttClient.subscribe(topicConfig);
        mqttClient.subscribe(topicTarget);
        mqttClient.subscribe(topicCmd);
        mqttClient.subscribe(topicTest);
        mqttClient.subscribe(topicCal);
        mqttClient.subscribe(topicDebug);

        // Secciones individuales para relays/MOSFET (acepta ambos topics).
        char topicSections[64];
        sprintf(topicSections, "agp/quantix/%s/sections", uid);
        mqttClient.subscribe(topicSections);

        Serial.printf("📡 Suscrito a secciones: %s\n", topicSections);

        // ANUNCIO
        StaticJsonDocument<256> ann;
        ann["uid"] = uid;
        ann["ip"] = WiFi.localIP().toString();
        ann["type"] = "QuantiX";
        ann["fw"] = FW_VERSION;
        ann["motors"] = MDL.SensorCount;
        ann["uptime"] = millis() / 1000;

        char buffer[256];
        serializeJson(ann, buffer);
        mqttClient.publish("agp/quantix/announcement", buffer, true); // retained

        return true;
    }
    return false;
}

void mqttLoop()
{
    // --- WiFi caído: reconectar o fallback AP ---
    if (WiFi.status() != WL_CONNECTED && !apFallbackActive)
    {
        wifiFailCount++;
        if (wifiFailCount >= 10)
        {
            // Después de 10 fallos → AP fallback para que el usuario reconfigure
            Serial.println("⚠️ WiFi caído 10 veces → activando AP de emergencia");
            WiFi.mode(WIFI_AP_STA); // Mantener STA intentando + AP para config
            char apName[32];
            snprintf(apName, sizeof(apName), "QX-%s", uid + 3);
            WiFi.softAP(apName, MDL.APpassword);
            Serial.print("📱 AP emergencia: "); Serial.println(apName);
            apFallbackActive = true;
            wifiFailCount = 0;
        }
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        // Si estábamos en AP fallback y WiFi volvió, desactivar AP
        if (apFallbackActive)
        {
            Serial.println("✅ WiFi reconectado → desactivando AP emergencia");
            WiFi.mode(WIFI_STA);
            apFallbackActive = false;
        }
        wifiFailCount = 0;

        if (!mqttClient.connected())
        {
            unsigned long now = millis();
            if (now - lastMqttReconnectAttempt > mqttReconnectInterval)
            {
                lastMqttReconnectAttempt = now;
                if (mqttReconnect())
                {
                    lastMqttReconnectAttempt = 0;
                    mqttFailCount = 0;
                }
                else
                {
                    mqttFailCount++;
                    if (mqttFailCount >= 10)
                    {
                        Serial.println("⚠️ MQTT falló 10 veces → activando AP para reconfigurar broker");
                        WiFi.mode(WIFI_AP_STA);
                        char apName[32];
                        snprintf(apName, sizeof(apName), "QX-%s", uid + 3);
                        WiFi.softAP(apName, MDL.APpassword);
                        apFallbackActive = true;
                        mqttFailCount = 0;
                    }
                }
            }
        }
        else
        {
            mqttClient.loop();
            mqttFailCount = 0;
        }
    }
}

void sendMQTTStatus(byte ID)
{
    if (!mqttClient.connected())
        return;

    StaticJsonDocument<350> doc; // Aumentamos un poco el tamaño por seguridad

    doc["uid"] = uid;
    doc["id"] = ID; // 0 o 1 interno de la placa
    doc["id_placa"] = MDL.ID;

    // Datos de tiempo real
    doc["rpm"] = (int)Sensor[ID].RPM;
    doc["pulsos"] = Sensor[ID].TotalPulses;
    doc["pwm"] = (int)Sensor[ID].PWM;
    doc["isr_total"] = TotalInterrupts[ID];
    doc["isr_filtered"] = Sensor[ID].TotalPulses; // solo los que pasaron el filtro
    doc["ppr"] = Sensor[ID].PulsesPerRev;
    doc["pulse_min"] = Sensor[ID].PulseMin;
    doc["motor_type"] = (uint8_t)Sensor[ID].MotorType;

    // --- DATOS CLAVE PARA DOSIS REAL ---
    doc["pps_real"] = Sensor[ID].Hz;          // Frecuencia actual del encoder
    doc["pps_target"] = Sensor[ID].TargetUPM; // Frecuencia buscada por el PID
    doc["meter_cal"] = Sensor[ID].MeterCal;   // Enviamos su propia constante de calibración
    // ------------------------------------

    if (Sensor[ID].CalibActive)
        doc["calibrando"] = true;

    float load = (Sensor[ID].PWM / 4095.0f) * 100.0f;
    if (load > 100)
        load = 100;
    doc["load_pct"] = (int)load;

    char buffer[512];
    serializeJson(doc, buffer);
    mqttClient.publish(topicStatus, buffer);
}