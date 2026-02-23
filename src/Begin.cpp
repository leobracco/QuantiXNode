#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "Globals.h"
#include "Structs.h"

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
    Serial.println(F("\n\n--- INICIANDO HARDWARE QUANTIX ---"));

    // 1. Iniciar I2C
    Wire.begin(); 

    // 2. Iniciar PCA9685 (Driver PWM)
    PWMServoDriver.begin();
    PWMServoDriver.setOscillatorFrequency(27000000); 
    PWMServoDriver.setPWMFreq(1000); 
    Serial.println(F("✅ PCA9685 Configurado"));

    // 3. Cargar Configuración
    LoadData();
    LoadNetworks(); // Cargar datos de red también

    if (!CheckPins()) Serial.println("⚠️ ERROR: Pines inválidos detectados.");

    // 4. Configurar Pines y Hardware
    if (MDL.WorkPin != NC) pinMode(MDL.WorkPin, INPUT_PULLUP);

    for (int i = 0; i < MDL.SensorCount; i++) {
        
        // A. Configurar Encoder (Interrupciones)
        if (Sensor[i].FlowPin != NC) {
            pinMode(Sensor[i].FlowPin, INPUT_PULLUP);
            
            // Asignar ISR según el ID del motor
            if (i == 0) {
                attachInterrupt(digitalPinToInterrupt(Sensor[i].FlowPin), ISR_Sensor0, RISING);
                Serial.printf("✅ Encoder M0 activo en Pin %d\n", Sensor[i].FlowPin);
            }
            else if (i == 1) {
                attachInterrupt(digitalPinToInterrupt(Sensor[i].FlowPin), ISR_Sensor1, RISING);
                Serial.printf("✅ Encoder M1 activo en Pin %d\n", Sensor[i].FlowPin);
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
            ledcSetup(ch1, 1000, 12); // 1000 Hz, 8 bit
            ledcAttachPin(Sensor[i].IN1, ch1); 
        }
        if (Sensor[i].IN2 != NC) {
            ledcSetup(ch2, 1000, 12);
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
    delay(500); // Pequeña pausa para estabilizar

    Serial.println("--- Conectando WiFi ---");
    
    bool connected = false;

    // Intentar conectar a Router (Station Mode)
    if (MDLnetwork.WifiModeUseStation && strlen(MDLnetwork.SSID) > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(MDLnetwork.SSID, MDLnetwork.Password);
        Serial.print("Conectando a: "); Serial.print(MDLnetwork.SSID);
        
        uint32_t start = millis();
        // Esperamos 10 seg max
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(500); Serial.print(".");
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n✅ WiFi Conectado. IP: " + WiFi.localIP().toString());
            connected = true;
        } else {
            Serial.println("\n❌ Fallo conexión. Iniciando AP.");
        }
    }

    // Si falló o no se pidió Station, levantar AP
    if (!connected) {
        WiFi.mode(WIFI_AP);
        if (strlen(MDL.APpassword) < 8) strcpy(MDL.APpassword, "12345678");
        
        WiFi.softAP("Quantix_AP", MDL.APpassword);
        Serial.println("✅ AP Creado: Quantix_AP");
        Serial.print("IP AP: "); Serial.println(WiFi.softAPIP());
    }

    // --- INICIAR MQTT ---
    // Importante llamarlo al final del setup
    initMQTT();
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
        s["MeterCal"] = Sensor[i].MeterCal;
        s["TargetUPM"] = Sensor[i].TargetUPM;
        s["PulseSampleSize"] = Sensor[i].PulseSampleSize;
    }

    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        Serial.println("Error escribiendo config.json");
        return;
    }
    serializeJson(doc, file);
    file.close();
    Serial.println("Config guardada.");
}

void LoadData()
{
    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("Config no existe, cargando default.");
        SetDefault();
        SaveData();
        return;
    }

    File file = LittleFS.open(CONFIG_FILE, "r");
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.println("JSON Error, cargando default.");
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
        Sensor[i].MeterCal = sensors[i]["MeterCal"] | 50.0;
        Sensor[i].TargetUPM = sensors[i]["TargetUPM"] | 0.0;
        
        Sensor[i].PulseSampleSize = sensors[i]["PulseSampleSize"] | 5; 
    }
    Serial.println("Datos cargados desde JSON.");
}
void SaveNetworks()
{
    StaticJsonDocument<512> doc;
    doc["SSID"] = MDLnetwork.SSID;
    doc["Password"] = MDLnetwork.Password;
    doc["Mode"] = MDLnetwork.WifiModeUseStation;

    File file = LittleFS.open(NETWORK_FILE, "w");
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
    StaticJsonDocument<512> doc;
    deserializeJson(doc, file);
    file.close();

    strlcpy(MDLnetwork.SSID, doc["SSID"] | "", sizeof(MDLnetwork.SSID));
    strlcpy(MDLnetwork.Password, doc["Password"] | "", sizeof(MDLnetwork.Password));
    MDLnetwork.WifiModeUseStation = doc["Mode"] | false;
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
    strcpy(MDL.APpassword, "12345678");

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

        Sensor[i].PulsesPerRev = 24; // Default: 24 pulsos = 1 vuelta
        Sensor[i].RPM = 0;
        Sensor[i].CalibActive = false;
        Sensor[i].TotalPulses = 0;
    }
    
    strcpy(MDLnetwork.SSID, "");
    strcpy(MDLnetwork.Password, "");
}