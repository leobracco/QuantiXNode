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

// Identidad Única
char uid[13];

// Tópicos Dinámicos
char topicStatus[64], topicTarget[64], topicConfig[64], topicCmd[64], topicTest[64], topicCal[64];

void obtenerUID()
{
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uid, sizeof(uid), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    Serial.printf("🆔 UID del Dispositivo: %s\n", uid);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    String t = String(topic);
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
                        Sensor[id].MinPWM = cal["pwm_min"] | 40;
                        Sensor[id].MaxPWM = cal["pwm_max"] | 255;
                    }
                    Sensor[id].MeterCal = c["meter_cal"] | Sensor[id].MeterCal;
                    ResetPIDState(id);
                }
            }
            SaveData();
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
        }
    }

    // --- C. PROCESAR TEST (Slider web) ---
    else if (t == String(topicTest))
    {
        const char *cmd = doc["cmd"] | "stop";
        int id = doc["id"] | 0;
        int pwmVal = doc["pwm"] | 0;
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

    // --- D. PROCESAR CALIBRACIÓN (Muestra controlada) ---
    // --- D. PROCESAR CALIBRACIÓN ---
    // --- D. PROCESAR CALIBRACIÓN (Con memoria de PWM) ---
    else if (t == String(topicCal))
    {
        int id = doc["id"] | 0;
        long pulsosMeta = doc["pulsos"] | 0;
        int pwmVal = doc["pwm"] | 1000; // Recibimos el PWM de la web
        const char *cmd = doc["cmd"] | "stop";

        if (id < MDL.SensorCount)
        {
            if (strcmp(cmd, "start") == 0 && pulsosMeta > 0)
            {
                Sensor[id].CalibActive = false;

                noInterrupts();
                Sensor[id].TotalPulses = 0;
                interrupts();

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

        // ANUNCIO
        StaticJsonDocument<200> ann;
        ann["uid"] = uid;
        ann["ip"] = WiFi.localIP().toString();
        ann["type"] = "MOTOR";

        char buffer[200];
        serializeJson(ann, buffer);
        mqttClient.publish("agp/quantix/announcement", buffer);

        return true;
    }
    return false;
}

void mqttLoop()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!mqttClient.connected())
        {
            unsigned long now = millis();
            if (now - lastMqttReconnectAttempt > mqttReconnectInterval)
            {
                lastMqttReconnectAttempt = now;
                if (mqttReconnect())
                {
                    lastMqttReconnectAttempt = 0;
                }
            }
        }
        else
        {
            mqttClient.loop();
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

    char buffer[350];
    serializeJson(doc, buffer);
    mqttClient.publish(topicStatus, buffer);
}