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
extern void initMQTT();
extern void obtenerUID();
extern char uid[13];

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

    // 2. Iniciar PCA9685 (Driver PWM) — RC15 PCB address 0x55
    Serial.print(F("Buscando PCA9685 en 0x55... "));
    Wire.beginTransmission(0x55);
    bool pca9685Found = (Wire.endTransmission() == 0);

    if (pca9685Found)
    {
        PWMServoDriver.begin();
        PWMServoDriver.setPWMFreq(200); // 200 Hz como SK21/RateControl

        // OE (Output Enable) — PIN 27, LOW = habilitado.
        pinMode(27, OUTPUT);
        digitalWrite(27, LOW);

        Serial.println(F("✅ PCA9685 OK + OE=LOW"));

        // TEST: prender TODOS los canales 2 seg.
        Serial.println(F("🧪 TEST: SA1-SA8 ON por 2 seg..."));
        for (int i = 0; i < 16; i++)
            PWMServoDriver.setPWM(i, 4096, 0);
        delay(2000);
        for (int i = 0; i < 16; i++)
            PWMServoDriver.setPWM(i, 0, 4096);
        Serial.println(F("🧪 TEST: apagado."));
    }
    else
    {
        Serial.println(F("❌ PCA9685 NO encontrado en 0x55!"));
        // Intentar 0x40 (default).
        Wire.beginTransmission(0x40);
        if (Wire.endTransmission() == 0)
            Serial.println(F("   → Encontrado en 0x40 (cambiar address en main.cpp)"));
    }

    // 3. Cargar Configuración
    LoadData();
    LoadNetworks(); // Cargar datos de red también

    if (!CheckPins()) Serial.println("⚠️ ERROR: Pines inválidos detectados.");

    // 3b. Cachear filtro de pulsos ANTES de habilitar interrupciones
    CachePulseFilter();

    // 4. Configurar Pines y Hardware
    if (MDL.WorkPin != NC) pinMode(MDL.WorkPin, INPUT_PULLUP);

    for (int i = 0; i < MDL.SensorCount; i++) {
        
        // A. Configurar Encoder (Interrupciones)
        // LPD3806-600BM: salida push-pull, no necesita pull-up.
        // IMPORTANTE: Si el encoder es 5V, usar divisor resistivo (10k+20k) al pin.
        if (Sensor[i].FlowPin != NC) {
            pinMode(Sensor[i].FlowPin, INPUT_PULLUP);

            // Asignar ISR según el ID del motor
            if (i == 0) {
                attachInterrupt(digitalPinToInterrupt(Sensor[i].FlowPin), ISR_Sensor0, RISING);
                Serial.printf("✅ Sensor M0 en Pin %d (PPR:%d) RISING\n", Sensor[i].FlowPin, Sensor[i].PulsesPerRev);
            }
            else if (i == 1) {
                attachInterrupt(digitalPinToInterrupt(Sensor[i].FlowPin), ISR_Sensor1, RISING);
                Serial.printf("✅ Encoder M1 activo en Pin %d (PPR:%d)\n", Sensor[i].FlowPin, Sensor[i].PulsesPerRev);
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

    // --- INICIO DE RED WIFI con reintentos y fallback AP ---
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    Serial.println("--- Conectando WiFi ---");

    bool connected = false;

    if (MDLnetwork.WifiModeUseStation && strlen(MDLnetwork.SSID) > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(MDLnetwork.SSID, MDLnetwork.Password);
        Serial.print("Conectando a: "); Serial.print(MDLnetwork.SSID);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
            delay(500); Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n✅ WiFi Conectado. IP: " + WiFi.localIP().toString());
            connected = true;
        } else {
            Serial.println("\n❌ Fallo conexión WiFi.");
        }
    }

    // Si no conectó → AP con portal de configuración
    if (!connected) {
        WiFi.mode(WIFI_AP);
        if (strlen(MDL.APpassword) < 8) strcpy(MDL.APpassword, "12345678");

        // AP con nombre identificable por UID
        char apName[32];
        extern char uid[13];
        obtenerUID(); // Asegurar que el UID esté calculado
        snprintf(apName, sizeof(apName), "QX-%s", uid + 3); // QX-XXXXXXXX

        WiFi.softAP(apName, MDL.APpassword);
        Serial.print("✅ AP Portal: "); Serial.println(apName);
        Serial.print("IP AP: "); Serial.println(WiFi.softAPIP());
        Serial.println("📱 Conectá al AP y entrá a 192.168.4.1 para configurar WiFi");
    }

    // --- REGISTRAR RUTAS WEB ---
    extern void HandleRoot();
    extern void HandlePage1();
    extern void HandlePage2();

    server.on("/", HTTP_GET, HandleRoot);
    server.on("/", HTTP_POST, HandleRoot);
    server.on("/p1", HTTP_GET, HandlePage1);
    server.on("/p1", HTTP_POST, HandlePage1);
    server.on("/p2", HTTP_GET, HandlePage2);
    server.begin();
    Serial.println("✅ WebServer iniciado en puerto 80");

    // --- INICIAR MQTT ---
    initMQTT();
}

// --- GESTIÓN JSON ---

void SaveData()
{
    DynamicJsonDocument doc(4096);
    doc["MDL"]["ID"] = MDL.ID;
    doc["MDL"]["SensorCount"] = MDL.SensorCount;
    doc["MDL"]["RelayControl"] = MDL.RelayControl;
    doc["MDL"]["Is3Wire"] = MDL.Is3Wire;
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
        s["MotorType"] = (uint8_t)Sensor[i].MotorType;
        s["Kp"] = Sensor[i].Kp;
        s["Ki"] = Sensor[i].Ki;
        s["Kd"] = Sensor[i].Kd;
        s["MinPWM"] = Sensor[i].MinPWM;
        s["MaxPWM"] = Sensor[i].MaxPWM;
        s["MaxIntegral"] = Sensor[i].MaxIntegral;
        s["PIDtime"] = Sensor[i].PIDtime;
        s["Deadband"] = Sensor[i].Deadband;
        s["BrakePoint"] = Sensor[i].BrakePoint;
        s["SlewRate"] = Sensor[i].SlewRate;
        s["SlewRatePerSec"] = Sensor[i].SlewRatePerSec;
        s["TargetSlewHzPerSec"] = Sensor[i].TargetSlewHzPerSec;
        s["MeterCal"] = Sensor[i].MeterCal;
        s["TargetUPM"] = Sensor[i].TargetUPM;
        s["PulseSampleSize"] = Sensor[i].PulseSampleSize;
        s["PulsesPerRev"] = Sensor[i].PulsesPerRev;
        s["PulseMin"] = Sensor[i].PulseMin;
        s["MaxHz"] = Sensor[i].MaxHz;
        s["FFGain"] = Sensor[i].FFGain;
        s["Alpha"] = Sensor[i].Alpha;
        s["HydDeadZonePWM"] = Sensor[i].HydDeadZonePWM;
        s["HydHysteresis"] = Sensor[i].HydHysteresis;
        s["HydPWMFreq"] = Sensor[i].HydPWMFreq;
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
    DynamicJsonDocument doc(4096);
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
    MDL.Is3Wire = doc["MDL"]["Is3Wire"] | true;
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
        
        Sensor[i].MotorType = (MotorType_t)(sensors[i]["MotorType"] | 0);
        Sensor[i].Kp = sensors[i]["Kp"] | 80.0;
        Sensor[i].Ki = sensors[i]["Ki"] | 30.0;
        Sensor[i].Kd = sensors[i]["Kd"] | 0.0;
        Sensor[i].MinPWM = sensors[i]["MinPWM"] | 600;
        Sensor[i].MaxPWM = sensors[i]["MaxPWM"] | 4095;
        Sensor[i].MaxIntegral = sensors[i]["MaxIntegral"] | 1200.0;
        Sensor[i].PIDtime = sensors[i]["PIDtime"] | 50;

        Sensor[i].Deadband = sensors[i]["Deadband"] | 2;
        Sensor[i].BrakePoint = sensors[i]["BrakePoint"] | 15;
        Sensor[i].SlewRate = sensors[i]["SlewRate"] | 40;
        Sensor[i].SlewRatePerSec = sensors[i]["SlewRatePerSec"] | 5000.0;
        Sensor[i].TargetSlewHzPerSec = sensors[i]["TargetSlewHzPerSec"] | 50.0;
        Sensor[i].MeterCal = sensors[i]["MeterCal"] | 50.0;
        Sensor[i].TargetUPM = sensors[i]["TargetUPM"] | 0.0;

        Sensor[i].PulseSampleSize = sensors[i]["PulseSampleSize"] | 5;
        Sensor[i].PulsesPerRev = sensors[i]["PulsesPerRev"] | 24;
        Sensor[i].PulseMin = sensors[i]["PulseMin"] | 2000;
        Sensor[i].MaxHz = sensors[i]["MaxHz"] | 40.0;
        Sensor[i].FFGain = sensors[i]["FFGain"] | 1.0;
        Sensor[i].Alpha = sensors[i]["Alpha"] | 0.4;

        Sensor[i].HydDeadZonePWM = sensors[i]["HydDeadZonePWM"] | 1200.0;
        Sensor[i].HydHysteresis = sensors[i]["HydHysteresis"] | 8.0;
        Sensor[i].HydPWMFreq = sensors[i]["HydPWMFreq"] | 150;

        // Migración: si SlewRatePerSec=0 pero SlewRate viejo existe, convertir
        if (Sensor[i].SlewRatePerSec <= 0 && Sensor[i].SlewRate > 0)
            Sensor[i].SlewRatePerSec = Sensor[i].SlewRate * (1000.0f / Sensor[i].PIDtime);
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
        Sensor[i].MotorType = MOTOR_ELECTRIC;

        // PID con feedforward (ganancias escaladas a PWM 12-bit)
        Sensor[i].Kp = 80.0;
        Sensor[i].Ki = 30.0;
        Sensor[i].Kd = 0.0;
        Sensor[i].MinPWM = 600;
        Sensor[i].MaxPWM = 4095;
        Sensor[i].MaxIntegral = 1200.0;
        Sensor[i].PIDtime = 50;

        // Feedforward
        Sensor[i].MaxHz = 40.0;
        Sensor[i].FFGain = 1.0;
        Sensor[i].Alpha = 0.4;

        // Slew rate en PWM/segundo
        Sensor[i].SlewRatePerSec = 5000.0;
        Sensor[i].TargetSlewHzPerSec = 50.0;
        Sensor[i].SlewRate = 40; // legacy

        // Ajuste Fino
        Sensor[i].Deadband = 2;
        Sensor[i].BrakePoint = 15;

        // Hidráulico defaults
        Sensor[i].HydDeadZonePWM = 1200.0;
        Sensor[i].HydHysteresis = 8.0;
        Sensor[i].HydPWMFreq = 150;

        // Lectura
        Sensor[i].PulseSampleSize = 5;
        Sensor[i].PulsesPerRev = 24;
        Sensor[i].PulseMin = 2000;  // µs - filtro anti-rebote inductivo

        // Estado Inicial
        Sensor[i].TargetUPM = 0;
        Sensor[i].ManualAdjust = 0;
        Sensor[i].AutoOn = true;
        Sensor[i].RPM = 0;
        Sensor[i].CalibActive = false;
        Sensor[i].TotalPulses = 0;
    }
    
    strcpy(MDLnetwork.SSID, "");
    strcpy(MDLnetwork.Password, "");
}