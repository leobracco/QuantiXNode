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

void obtenerUID() {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(uid, sizeof(uid), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
    Serial.printf("🆔 UID del Dispositivo: %s\n", uid);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) return;

    String t = String(topic);

    // 1. CONFIGURACIÓN (Llega por UID)
    if (t == String(topicConfig)) {
        // En tu estructura de server.js envias un array "configs", aquí asumimos estructura simple
        // Si quieres configurar PID indidivual, debes leer "idx" o "id"
        // Por compatibilidad simple:
        
        if(doc["id_logico"]) {
            MDL.ID = doc["id_logico"];
            SaveData();
            Serial.printf("✅ Configurado ID Placa: %d\n", MDL.ID);
        }
        
        // Configuración PID (Ejemplo básico, idealmente iterar sobre configs array)
        int id = doc["idx"] | 0; // Si viene especificado
        if (id < MDL.SensorCount) {
             if(doc["kp"]) Sensor[id].Kp = doc["kp"];
             if(doc["ki"]) Sensor[id].Ki = doc["ki"];
             // ... otros parámetros
             ResetPIDState(id);
        }
    }
    
    // 2. TARGET (Dosis)
    else if (t == String(topicTarget)) {
        // Aquí asumimos que el mensaje target trae info para qué motor es o es general
        // Si es general, aplicamos a todos o parseamos arrays.
        // Simplificación:
        float pps = doc["pps"] | 0.0f;
        int id = doc["id"] | 0;
        
        if (id < MDL.SensorCount) {
             Sensor[id].TargetUPM = pps;
             bool seccionOn = doc["seccion_on"] | true;
             if (!seccionOn) Sensor[id].TargetUPM = 0;
             Sensor[id].FlowEnabled = seccionOn;
        }
    }

    // 3. TEST MANUAL
    else if (t == String(topicTest)) {
        
        const char* cmd = doc["cmd"] | "stop"; 
        int targetID = doc["id"] | 0;          
        int pwmVal = doc["pwm"] | 0;

        if (targetID >= 0 && targetID < MDL.SensorCount) {

            if (strcmp(cmd, "start") == 0) {
                Sensor[targetID].AutoOn = false; 
                Sensor[targetID].ManualAdjust = pwmVal; 
                
                // Limpiar PID
                ResetPIDState(targetID);

                // Mover hardware
                SetPWM(targetID, pwmVal);

                Serial.printf("🔧 TEST START: Motor %d a PWM %d\n", targetID, pwmVal);
            } 
            else {
                // STOP
                Sensor[targetID].AutoOn = true; 
                Sensor[targetID].ManualAdjust = 0;
                ResetPIDState(targetID);
                SetPWM(targetID, 0);

                Serial.printf("🔧 TEST STOP: Motor %d\n", targetID);
            }
        }
    }
    
    // 4. CALIBRACIÓN (Dosis exacta)
    else if (t == String(topicCal)) {
        int id = doc["id"] | 0;
        long pulsosMeta = doc["pulsos"] | 0; 
        int pwm = doc["pwm"] | 0;            
        const char* cmd = doc["cmd"] | "stop";

        if (id >= 0 && id < MDL.SensorCount) {
            
            if (strcmp(cmd, "start") == 0 && pulsosMeta > 0) {
                Sensor[id].AutoOn = false;      
                Sensor[id].ManualAdjust = pwm;  
                
                // RESETEAR CONTADOR A CERO
                noInterrupts(); 
                Sensor[id].TotalPulses = 0; 
                interrupts();

                Sensor[id].CalibTargetPulses = pulsosMeta; 
                Sensor[id].CalibActive = true;

                SetPWM(id, pwm);
                Serial.printf("⚖️ START CAL M%d: Meta %ld pulsos\n", id, pulsosMeta);
            } 
            else {
                // ABORTAR
                Sensor[id].CalibActive = false;
                Sensor[id].ManualAdjust = 0;
                Sensor[id].AutoOn = true; 
                SetPWM(id, 0);
                Serial.printf("⚖️ STOP CAL M%d\n", id);
            }
        }
    }
}

void initMQTT() {
    obtenerUID();
    
    // Configuramos los tópicos basados en el UID físico
    sprintf(topicStatus, "agp/quantix/%s/status_live", uid);
    sprintf(topicTarget, "agp/quantix/%s/target", uid);
    sprintf(topicConfig, "agp/quantix/%s/config", uid);
    sprintf(topicCmd,    "agp/quantix/%s/cmd", uid);
    sprintf(topicTest,   "agp/quantix/%s/test", uid);
    sprintf(topicCal,    "agp/quantix/%s/cal", uid);

    // AJUSTA LA IP AQUÍ A LA DE TU SERVIDOR/PC
    mqttClient.setServer(IPAddress(192, 168, 1, 11), 1883);
    mqttClient.setCallback(mqttCallback);
}

boolean mqttReconnect() {
    if (mqttClient.connect(uid)) { 
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

void mqttLoop() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) {
            unsigned long now = millis();
            if (now - lastMqttReconnectAttempt > mqttReconnectInterval) {
                lastMqttReconnectAttempt = now;
                if (mqttReconnect()) {
                    lastMqttReconnectAttempt = 0;
                }
            }
        } else {
            mqttClient.loop();
        }
    }
}

// --- FUNCIÓN CORREGIDA PARA EVITAR MEZCLA DE MOTORES ---
void sendMQTTStatus(byte ID) {
    if (!mqttClient.connected()) return;

    StaticJsonDocument<300> doc;
    
    doc["uid"] = uid;           
    
    // CORRECCIÓN CRÍTICA:
    // Enviamos el 'id' interno (0 o 1) para que el JS sepa cual motor es.
    // Antes enviabas MDL.ID que es igual para ambos motores de la placa.
    doc["id"] = ID;  
    
    // Opcional: También enviamos el de la placa por si sirve
    doc["id_placa"] = MDL.ID;

    // Datos de tiempo real
    doc["rpm"] = (int)Sensor[ID].RPM;            
    doc["pulsos"] = Sensor[ID].TotalPulses;      
    doc["pwm"] = (int)Sensor[ID].PWM;
    doc["pps_real"] = Sensor[ID].Hz;
    doc["pps_target"] = Sensor[ID].TargetUPM;
    
    // Estado de Calibración
    if(Sensor[ID].CalibActive) doc["calibrando"] = true;

    // Cálculo de Carga del Motor (%)
    // CORREGIDO: Usamos 4095.0f porque ahora estamos en 12 bits
    float load = (Sensor[ID].PWM / 4095.0f) * 100.0f;
    if (load > 100) load = 100;
    doc["load_pct"] = (int)load;

    char buffer[300];
    serializeJson(doc, buffer);
    mqttClient.publish(topicStatus, buffer);
}