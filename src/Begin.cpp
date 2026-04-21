#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#include "Globals.h"
#include "Structs.h"
#include "Constants.h"
#include "Log.h"

// Variables Externas
extern const uint16_t InoID;
extern Adafruit_PWMServoDriver PWMServoDriver;
extern PCF8574 PCF;

extern bool PCF_found;
extern bool GoodPins;
extern void initMQTT(); // Importante: Declarada para usarla al final

// Archivos de configuración
const char* CONFIG_FILE = "/config.json";
const char* NETWORK_FILE = "/network.json";

// Pines válidos para el ESP32
uint8_t ValidPins0[] = { 0,2,4,13,14,15,16,17,21,22,25,26,27,32,33 };

// Prototipos Locales
bool CheckPins();
void SetDefault();
void SaveData();
void LoadData();
void SaveNetworks();
void LoadNetworks();

void DoSetup() {
    LOG_I("boot", "Iniciando hardware QuantiX");

    // 1. Iniciar I2C
    Wire.begin();

    // 2. Iniciar PCA9685 (Driver PWM)
    PWMServoDriver.begin();
    PWMServoDriver.setOscillatorFrequency(27000000);
    PWMServoDriver.setPWMFreq(quantix::constants::PWM_FREQ_HZ);
    LOG_I("hw", "PCA9685 configurado");

    // 3. Cargar Configuración
    LoadData();
    LoadNetworks();

    if (!CheckPins())
        LOG_W("cfg", "Pines inválidos detectados");

    // 4. Configurar Pines y Hardware
    if (MDL.WorkPin != NC) pinMode(MDL.WorkPin, INPUT_PULLUP);

    for (int i = 0; i < MDL.SensorCount; i++) {
        
        // A. Configurar Encoder (Interrupciones)
        if (Sensor[i].FlowPin != NC) {
            pinMode(Sensor[i].FlowPin, INPUT_PULLUP);
            
            // Asignar ISR según el ID del motor
            if (i == 0) {
                attachInterrupt(digitalPinToInterrupt(Sensor[i].FlowPin), ISR_Sensor0, RISING);
                LOG_I("hw", "Encoder M0 activo en pin %u", Sensor[i].FlowPin);
            }
            else if (i == 1) {
                attachInterrupt(digitalPinToInterrupt(Sensor[i].FlowPin), ISR_Sensor1, RISING);
                LOG_I("hw", "Encoder M1 activo en pin %u", Sensor[i].FlowPin);
            }
        }

        // B. Configurar Pines de Salida
        if (Sensor[i].DirPin != NC) pinMode(Sensor[i].DirPin, OUTPUT);
        if (Sensor[i].PWMPin != NC) pinMode(Sensor[i].PWMPin, OUTPUT);

        // C. Configurar Canales PWM (LEDC) para el ESP32
        // ESTO FALTABA Y ES CRÍTICO PARA QUE EL MOTOR GIRE
        uint8_t ch1 = i * 2;     
        uint8_t ch2 = i * 2 + 1; 

        if (Sensor[i].IN1 != NC) {
            ledcSetup(ch1, quantix::constants::PWM_FREQ_HZ, quantix::constants::PWM_RESOLUTION_BITS);
            ledcAttachPin(Sensor[i].IN1, ch1);
        }
        if (Sensor[i].IN2 != NC) {
            ledcSetup(ch2, quantix::constants::PWM_FREQ_HZ, quantix::constants::PWM_RESOLUTION_BITS);
            ledcAttachPin(Sensor[i].IN2, ch2);
        }

        // D. Apagar motor por seguridad
        SetPWM(i, 0);
    }

    // Iniciar Expansor de Relés si existe
    if (MDL.RelayControl == 6 && PCF.begin()) PCF_found = true;

    // --- INICIO DE RED WIFI ---
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // En lugar de delay bloqueante usamos espera activa corta para dejar
    // que el stack de WiFi se estabilice sin bloquear tareas del sistema.
    const uint32_t wifiStabilizeUntil = millis() + 200;
    while (millis() < wifiStabilizeUntil) { yield(); }

    LOG_I("wifi", "Conectando WiFi");

    bool connected = false;

    // Intentar conectar a Router (Station Mode)
    if (MDLnetwork.WifiModeUseStation && strlen(MDLnetwork.SSID) > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(MDLnetwork.SSID, MDLnetwork.Password);
        LOG_I("wifi", "Conectando a SSID: %s", MDLnetwork.SSID);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED &&
               millis() - start < quantix::constants::AP_FALLBACK_TIMEOUT_MS) {
            // yield + feed watchdog mientras esperamos conexión (sin bloquear 500ms)
            delay(50);
            esp_task_wdt_reset();
        }

        if (WiFi.status() == WL_CONNECTED) {
            LOG_I("wifi", "Conectado, IP=%s", WiFi.localIP().toString().c_str());
            connected = true;
        } else {
            LOG_W("wifi", "Fallo conexión, iniciando AP");
        }
    }

    // Si falló o no se pidió Station, levantar AP
    if (!connected) {
        WiFi.mode(WIFI_AP);
        if (strlen(MDL.APpassword) < 8)
            strlcpy(MDL.APpassword, "12345678", sizeof(MDL.APpassword));

        WiFi.softAP("Quantix_AP", MDL.APpassword);
        LOG_I("wifi", "AP creado Quantix_AP, IP=%s", WiFi.softAPIP().toString().c_str());
    }
}

// --- GESTIÓN JSON ---

void SaveData()
{
    StaticJsonDocument<2048> doc;
    doc["MDL"]["ID"] = MDL.ID;
    doc["MDL"]["SensorCount"] = MDL.SensorCount;
    doc["MDL"]["RelayControl"] = MDL.RelayControl;
    doc["MDL"]["WorkPin"] = MDL.WorkPin;
    doc["MDL"]["APpassword"] = MDL.APpassword;
    doc["MDL"]["InvertFlow"] = MDL.InvertFlow;
    doc["MDL"]["InvertRelay"] = MDL.InvertRelay;
    doc["MDL"]["LogLevel"] = MDL.LogLevel;
    doc["MDL"]["HttpUser"] = MDL.HttpUser;
    doc["MDL"]["HttpPass"] = MDL.HttpPass;
    doc["MDL"]["WatchdogSec"] = MDL.WatchdogSec;
    doc["MDL"]["CommTimeoutMs"] = MDL.CommTimeoutMs;
    doc["MDL"]["WifiTimeoutMs"] = MDL.WifiTimeoutMs;
    
    JsonArray sensors = doc.createNestedArray("Sensors");
    for (int i = 0; i < 2; i++) { 
        JsonObject s = sensors.createNestedObject();
        s["FlowPin"] = Sensor[i].FlowPin;
        s["IN1"] = Sensor[i].IN1;
        s["IN2"] = Sensor[i].IN2;
        s["Kp"] = Sensor[i].Kp;
        s["Ki"] = Sensor[i].Ki;
        s["MinPWM"] = Sensor[i].MinPWM;
        s["MaxPWM"] = Sensor[i].MaxPWM;
        
        // GUARDAMOS MaxIntegral (CRÍTICO)
        s["MaxIntegral"] = Sensor[i].MaxIntegral;
        s["PIDtime"] = Sensor[i].PIDtime;
        s["Deadband"] = Sensor[i].Deadband;
        s["BrakePoint"] = Sensor[i].BrakePoint;
        s["SlewRate"] = Sensor[i].SlewRate;
        s["MeterCal"] = Sensor[i].GramosPorPulso;  // clave JSON histórica
        s["TargetUPM"] = Sensor[i].TargetUPM;
        s["PulseSampleSize"] = Sensor[i].PulseSampleSize;
    }

    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        LOG_E("cfg", "No se pudo abrir %s para escritura", CONFIG_FILE);
        return;
    }
    serializeJson(doc, file);
    file.close();
    LOG_I("cfg", "Config guardada");
}

void LoadData()
{
    if (!LittleFS.exists(CONFIG_FILE)) {
        LOG_W("cfg", "Config no existe, cargando defaults");
        SetDefault();
        SaveData();
        return;
    }

    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) {
        LOG_E("cfg", "No se pudo abrir %s, usando defaults", CONFIG_FILE);
        SetDefault();
        return;
    }
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        LOG_E("cfg", "JSON inválido (%s), cargando defaults", error.c_str());
        SetDefault();
        return;
    }

    MDL.ID = doc["MDL"]["ID"] | 1;
    MDL.SensorCount = doc["MDL"]["SensorCount"] | 2; // Default a 2 si falla
    MDL.RelayControl = doc["MDL"]["RelayControl"] | 0;
    MDL.WorkPin = doc["MDL"]["WorkPin"] | NC;
    strlcpy(MDL.APpassword, doc["MDL"]["APpassword"] | "12345678", sizeof(MDL.APpassword));
    MDL.InvertFlow = doc["MDL"]["InvertFlow"] | false;
    MDL.InvertRelay = doc["MDL"]["InvertRelay"] | false;

    MDL.LogLevel = doc["MDL"]["LogLevel"] | 2;  // INFO por defecto
    strlcpy(MDL.HttpUser, doc["MDL"]["HttpUser"] | "admin", sizeof(MDL.HttpUser));
    // Si no hay HttpPass configurado, reutilizamos APpassword para no dejar el portal abierto
    strlcpy(MDL.HttpPass, doc["MDL"]["HttpPass"] | MDL.APpassword, sizeof(MDL.HttpPass));
    MDL.WatchdogSec = doc["MDL"]["WatchdogSec"] | 5;
    MDL.CommTimeoutMs = doc["MDL"]["CommTimeoutMs"] | 4000;
    MDL.WifiTimeoutMs = doc["MDL"]["WifiTimeoutMs"] | 30000;

    JsonArray sensors = doc["Sensors"];
    for (int i = 0; i < 2; i++) {
        // --- DEFAULTS INTELIGENTES SEGÚN ID ---
        uint8_t defFlow = (i == 0) ? 17 : 16;
        uint8_t defIN1  = (i == 0) ? 32 : 25;
        uint8_t defIN2  = (i == 0) ? 33 : 26;

        Sensor[i].FlowPin = sensors[i]["FlowPin"] | defFlow;
        Sensor[i].IN1 = sensors[i]["IN1"] | defIN1;
        Sensor[i].IN2 = sensors[i]["IN2"] | defIN2;
        
        Sensor[i].Kp = sensors[i]["Kp"] | 2.5;
        Sensor[i].Ki = sensors[i]["Ki"] | 1.5;
        Sensor[i].MinPWM = sensors[i]["MinPWM"] | 40;
        Sensor[i].MaxPWM = sensors[i]["MaxPWM"] | 4095;
        
        // Cargamos variables críticas
        Sensor[i].MaxIntegral = sensors[i]["MaxIntegral"] | 4095.0;
        Sensor[i].PIDtime = sensors[i]["PIDtime"] | 50;

        Sensor[i].Deadband = sensors[i]["Deadband"] | 2;
        Sensor[i].BrakePoint = sensors[i]["BrakePoint"] | 15;
        Sensor[i].SlewRate = sensors[i]["SlewRate"] | 40;
        Sensor[i].GramosPorPulso = sensors[i]["MeterCal"] | 50.0;  // clave JSON histórica
        Sensor[i].TargetUPM = sensors[i]["TargetUPM"] | 0.0;
        
        Sensor[i].PulseSampleSize = sensors[i]["PulseSampleSize"] | 5;
    }
    LOG_I("cfg", "Config cargada desde JSON");
}
void SaveNetworks()
{
    StaticJsonDocument<512> doc;
    doc["SSID"] = MDLnetwork.SSID;
    doc["Password"] = MDLnetwork.Password;
    doc["Mode"] = MDLnetwork.WifiModeUseStation;

    doc["mqtt_host"] = MDLnetwork.MqttHost;
    doc["mqtt_port"] = MDLnetwork.MqttPort;
    doc["mqtt_user"] = MDLnetwork.MqttUser;
    doc["mqtt_pass"] = MDLnetwork.MqttPass;
    doc["mqtt_keepalive"] = MDLnetwork.MqttKeepAlive;

    File file = LittleFS.open(NETWORK_FILE, "w");
    if (!file)
    {
        LOG_E("net", "No se pudo abrir %s para escritura", NETWORK_FILE);
        return;
    }
    serializeJson(doc, file);
    file.close();
}

void LoadNetworks()
{
    if (!LittleFS.exists(NETWORK_FILE)) {
        SetDefault();
        return;
    }

    File file = LittleFS.open(NETWORK_FILE, "r");
    if (!file) {
        LOG_E("net", "No se pudo abrir %s, usando defaults", NETWORK_FILE);
        SetDefault();
        return;
    }
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        LOG_E("net", "JSON inválido (%s), usando defaults", err.c_str());
        SetDefault();
        return;
    }

    strlcpy(MDLnetwork.SSID, doc["SSID"] | "", sizeof(MDLnetwork.SSID));
    strlcpy(MDLnetwork.Password, doc["Password"] | "", sizeof(MDLnetwork.Password));
    MDLnetwork.WifiModeUseStation = doc["Mode"] | false;

    strlcpy(MDLnetwork.MqttHost, doc["mqtt_host"] | "", sizeof(MDLnetwork.MqttHost));
    MDLnetwork.MqttPort = doc["mqtt_port"] | 1883;
    strlcpy(MDLnetwork.MqttUser, doc["mqtt_user"] | "", sizeof(MDLnetwork.MqttUser));
    strlcpy(MDLnetwork.MqttPass, doc["mqtt_pass"] | "", sizeof(MDLnetwork.MqttPass));
    MDLnetwork.MqttKeepAlive = doc["mqtt_keepalive"] | 15;
}

// --- UTILIDADES ---

byte BuildModSenID(byte Mod_ID, byte Sen_ID)
{
    return ((Mod_ID << 4) | (Sen_ID & 0x0F));
}

bool CheckPins()
{
    bool Result = true;
    for (int i = 0; i < MDL.SensorCount; i++) {
        bool flowOk = false;
        bool pwmOk = false;
        for(uint8_t p : ValidPins0) if(Sensor[i].FlowPin == p) flowOk = true;
        if(Sensor[i].FlowPin == NC) flowOk = true;
        for(uint8_t p : ValidPins0) if(Sensor[i].IN1 == p) pwmOk = true;
        if(Sensor[i].IN1 == NC) pwmOk = true;
        if(!flowOk || !pwmOk) Result = false;
    }
    GoodPins = Result;
    return Result;
}

void SetDefault()
{
    MDL.ID = 1;
    MDL.SensorCount = 2; // <--- Activamos 2 motores
    MDL.RelayControl = 0;
    MDL.WorkPin = NC;
    strlcpy(MDL.APpassword, "12345678", sizeof(MDL.APpassword));

    // Defaults operacionales
    MDL.LogLevel = 2;  // INFO
    strlcpy(MDL.HttpUser, "admin", sizeof(MDL.HttpUser));
    strlcpy(MDL.HttpPass, MDL.APpassword, sizeof(MDL.HttpPass));
    MDL.WatchdogSec = 5;
    MDL.CommTimeoutMs = 4000;
    MDL.WifiTimeoutMs = 30000;

    // --- CONFIGURACIÓN DE PINES (HARDWARE) ---
    
    // Motor 0 (Semillas)
    Sensor[0].FlowPin = 17;
    Sensor[0].IN1 = 32;
    Sensor[0].IN2 = 33;

    // Motor 1 (Fertilizante) - PINES DIFERENTES
    Sensor[1].FlowPin = 16; // Pin RX2
    Sensor[1].IN1 = 25;     
    Sensor[1].IN2 = 26;     

    // --- CONFIGURACIÓN PID Y LÓGICA (PARA AMBOS) ---
    for (int i = 0; i < 2; i++) {
        Sensor[i].FlowEnabled = false;
        
        // PID Estándar
        Sensor[i].Kp = 2.5;     
        Sensor[i].Ki = 1.5;     
        Sensor[i].MinPWM = 150;
        Sensor[i].MaxPWM = 4095;
        Sensor[i].MaxIntegral = 4095.0; // Importante: Liberado
        Sensor[i].PIDtime = 50;        // Importante: 50ms
        
        // Ajuste Fino
        Sensor[i].Deadband = 2;
        Sensor[i].BrakePoint = 15;
        Sensor[i].SlewRate = 40;
        
        // Lectura
        Sensor[i].PulseSampleSize = 5; 
        
        // Estado Inicial
        Sensor[i].TargetUPM = 0;
        Sensor[i].ManualAdjust = 0;
        Sensor[i].AutoOn = true;

        Sensor[i].DientesPorVuelta = 24; // Default: engranaje de 24 dientes
        Sensor[i].RPM = 0;
        Sensor[i].CalibrandoAhora = false;
        Sensor[i].TotalPulses = 0;
    }
    
    strlcpy(MDLnetwork.SSID, "", sizeof(MDLnetwork.SSID));
    strlcpy(MDLnetwork.Password, "", sizeof(MDLnetwork.Password));
    MDLnetwork.WifiModeUseStation = false;

    // Defaults MQTT (antes estaban hardcodeados en MQTT_Custom.cpp)
    strlcpy(MDLnetwork.MqttHost, "", sizeof(MDLnetwork.MqttHost));
    MDLnetwork.MqttPort = 1883;
    strlcpy(MDLnetwork.MqttUser, "", sizeof(MDLnetwork.MqttUser));
    strlcpy(MDLnetwork.MqttPass, "", sizeof(MDLnetwork.MqttPass));
    MDLnetwork.MqttKeepAlive = 15;
}