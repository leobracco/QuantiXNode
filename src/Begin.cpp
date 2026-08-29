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
extern Adafruit_PWMServoDriver PWMServoDriver;
extern PCF8574 PCF;
extern bool PCF_found;
extern char uid[13];      // definido en MQTT_Custom.cpp
extern void obtenerUID(); // definido en MQTT_Custom.cpp
extern void wdtFeed();    // definido en main.cpp

const char *CONFIG_FILE = "/config.json";
const char *NETWORK_FILE = "/network.json";

// --- PINES SEGÚN TU ESQUEMÁTICO ---
#define PCB_SDA 27
#define PCB_SCL 26
#define PCA_OE 23 // Pin Output Enable del PCA9685

void SetDefault();
void LoadData();
void SaveData();
void LoadNetworks();

void DoSetup()
{
    Serial.println(F("\n--- INICIANDO HARDWARE FLOWX (PCB VERIFIED) ---"));

    // 1. Iniciar I2C con los pines de tu PCB (SDA=27, SCL=26)
    Wire.begin(PCB_SDA, PCB_SCL);
    Wire.setClock(400000);

    // 2. Configurar el pin OE del PCA9685 (Debe estar en LOW para habilitar salidas)
    pinMode(PCA_OE, OUTPUT);
    digitalWrite(PCA_OE, LOW);
    Serial.println(F("✅ PCA9685 Output Enable: LOW"));

    // 3. Iniciar PCA9685
    PWMServoDriver.begin();
    PWMServoDriver.setOscillatorFrequency(27000000);
    PWMServoDriver.setPWMFreq(1000);

    // 4. Cargar Configuración
    LoadData();
    LoadNetworks();

    // 5. Configurar Sensores y Actuadores (Pines SK21)
    if (CFG.FlowPin != NC)
    {
        pinMode(CFG.FlowPin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(CFG.FlowPin), ISR_Sensor0, RISING);
    }

    if (CFG.MasterPin != NC)
    {
        pinMode(CFG.MasterPin, OUTPUT);
        digitalWrite(CFG.MasterPin, LOW);
    }

    // 6. Configurar LEDC para Reguladora (Canales 0 y 1 para Pines 32 y 33)
    ledcSetup(0, 1000, 12);
    ledcAttachPin(CFG.RegPinA, 0);
    ledcSetup(1, 1000, 12);
    ledcAttachPin(CFG.RegPinB, 1);

    ledcWrite(0, 0);
    ledcWrite(1, 0);

    // 7. Configurar Secciones (Relays/Válvulas)
    for (int i = 0; i < 10; i++)
    {
        if (CFG.SectionPins[i][0] != NC)
            pinMode(CFG.SectionPins[i][0], OUTPUT);
        if (CFG.SectionPins[i][1] != NC)
            pinMode(CFG.SectionPins[i][1], OUTPUT);
    }

    // Iniciar expansores si existen
    if (MDL.RelayControl == 6 && PCF.begin())
        PCF_found = true;

    // --- WIFI: AP+STA PERMANENTE ---
    // Necesitamos el UID antes de armar el SSID del AP. obtenerUID() lo pobla
    // y también lo deja listo para initMQTT().
    obtenerUID();

    // Modo dual permanente. El AP nunca se cae: el operario siempre puede
    // entrar al portal del nodo desde el teléfono aunque la STA esté fallando.
    // apWatchdog() en MQTT_Custom.cpp lo re-asienta si el core lo tira.
    WiFi.mode(WIFI_AP_STA);
    delay(50);

    // SSID basado en UID (últimos 9 hex) — único por nodo, no choca en un
    // tractor con varios nodos cerca. Password configurable; default seguro.
    if (strlen(MDL.APpassword) < 8) strcpy(MDL.APpassword, "12345678");
    char apName[32];
    snprintf(apName, sizeof(apName), "FX-%s", uid + 3);

    // Orden CRÍTICO en ESP32 Arduino Core: softAP() PRIMERO, softAPConfig()
    // DESPUÉS. Al revés, en algunas versiones del core (2.x+) softAP() queda
    // sin efecto y el SSID nunca se broadcastea (el AP "está arriba" según
    // softAPIP() pero ningún cliente lo ve en el scan).
    bool apOk = WiFi.softAP(apName, MDL.APpassword, /*channel*/ 1, /*hidden*/ 0, /*max_conn*/ 4);
    delay(100); // que el DHCP server arranque antes de aceptar clientes

    // Forzamos el rango DHCP explícito 192.168.4.0/24 con .1 como gateway/AP.
    // Sin esto, en WIFI_AP_STA algunos clientes Android quedan en "obteniendo
    // IP" porque el pool DHCP no se inicializa hasta el primer asoc.
    IPAddress apIP(192, 168, 4, 1);
    IPAddress apGW(192, 168, 4, 1);
    IPAddress apMask(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apGW, apMask);

    Serial.printf("📡 AP %s: %s @ %s\n",
                  apOk ? "up" : "FAIL",
                  apName,
                  WiFi.softAPIP().toString().c_str());

    if (MDLnetwork.WifiModeUseStation && strlen(MDLnetwork.SSID) > 0)
    {
        WiFi.begin(MDLnetwork.SSID, MDLnetwork.Password);
        uint32_t start = millis();
        // Esperamos hasta 15s como QuantiX/VistaX. Alimentamos WDT en cada
        // delay para no falsificar el timeout de 30s del Task WDT.
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
        {
            delay(500);
            wdtFeed();
        }
        if (WiFi.status() == WL_CONNECTED)
            Serial.printf("📶 STA up: %s\n", WiFi.localIP().toString().c_str());
        else
            Serial.println("⚠ STA no conectó al boot — seguirá reintentando en background");
    }

    initMQTT();

    // ── Portal Web (estilo Agro Parallel) ──────────────────────────
    // Registro de rutas y arranque del WebServer. Las páginas viven en
    // Pages.cpp / handlers en GUI.cpp. El POST de Page2 (red+broker) cae
    // sobre "/" por convención del portal QuantiX (handleCredentials lo
    // detecta por la presencia de "prop1" o "bhost").
    server.on("/",   HTTP_GET,  HandleRoot);
    server.on("/",   HTTP_POST, HandleRoot);
    server.on("/p2", HTTP_GET,  HandlePage2);
    // Endpoint de diagnóstico — texto plano, sin formato. Sirve para descartar
    // problemas de routing / DHCP / browser captive-portal: si /ping no responde
    // tampoco, el problema no es de las páginas HTML sino más abajo (conexión,
    // IP del cliente, browser redirigiendo a otra URL, etc).
    server.on("/ping", HTTP_GET, []() {
        Serial.printf("[HTTP] GET /ping desde %s\n",
                      server.client().remoteIP().toString().c_str());
        server.send(200, "text/plain", "pong\n");
    });
    server.onNotFound([]() {
        Serial.printf("[HTTP] 404 %s desde %s\n",
                      server.uri().c_str(),
                      server.client().remoteIP().toString().c_str());
        server.send(200, "text/html", GetPage0());
    });
    server.begin();

    // Log de eventos WiFi del AP — vemos exactamente cuándo un cliente
    // se asocia, recibe IP, o se desasocia. Si "no aparece nada" es porque
    // el cliente no llega a asociarse o no recibe IP, lo vemos acá.
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
                Serial.println("📲 [AP] Cliente asociado");
                break;
            case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
                Serial.println("📴 [AP] Cliente desasociado");
                break;
            case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
                Serial.printf("✅ [AP] IP asignada a cliente: %s\n",
                              IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
                break;
            default: break;
        }
    });
    Serial.println(F("🌐 Portal web iniciado en :80 (AP: http://192.168.4.1)"));
}

// --- PERSISTENCIA JSON ---

void SaveConfig() { SaveData(); }

void SaveData()
{
    StaticJsonDocument<3072> doc;
    doc["MDL"]["ID"] = MDL.ID;
    doc["MDL"]["SensorCount"] = MDL.SensorCount;
    doc["MDL"]["RelayControl"] = MDL.RelayControl;

    JsonObject flow = doc.createNestedObject("FLOW");
    flow["meterCal"] = CFG.MeterCal;
    flow["is3Wire"] = CFG.Is3Wire;
    flow["invertRelay"] = CFG.InvertRelay;
    flow["invertMotor"] = CFG.InvertMotor;
    flow["masterPin"] = CFG.MasterPin;
    flow["flowPin"] = CFG.FlowPin;
    flow["regPinA"] = CFG.RegPinA;
    flow["regPinB"] = CFG.RegPinB;

    // Override per-sección del modo electroválvula (-1 = usar global).
    // Sirve para mezclar 2 y 3 cables en la misma barra sin tocar el default.
    JsonArray secMode = flow.createNestedArray("sectionIs3Wire");
    for (int i = 0; i < 10; i++)
        secMode.add((int)CFG.SectionIs3Wire[i]);

    JsonObject pid = flow.createNestedObject("pid");
    pid["kp"] = CFG.pid.kp;
    pid["ki"] = CFG.pid.ki;
    pid["pwmMin"] = CFG.pid.pwmMin;
    pid["pwmMinNeg"] = CFG.pid.pwmMinNeg;

    JsonArray sections = flow.createNestedArray("sections");
    for (int i = 0; i < 10; i++)
    {
        JsonArray s = sections.createNestedArray();
        s.add(CFG.SectionPins[i][0]);
        s.add(CFG.SectionPins[i][1]);
    }

    File file = LittleFS.open(CONFIG_FILE, "w");
    if (file)
    {
        serializeJson(doc, file);
        file.close();
    }
}

void LoadData()
{
    // Siempre seteo defaults primero. Luego pisamos con lo que haya en JSON.
    // Sin esto, los Sensor[] quedan en cero y el PID nunca actúa.
    SetDefault();

    if (!LittleFS.exists(CONFIG_FILE))
    {
        SaveData();
        return;
    }

    File file = LittleFS.open(CONFIG_FILE, "r");
    StaticJsonDocument<3072> doc;
    deserializeJson(doc, file);
    file.close();

    MDL.ID = doc["MDL"]["ID"] | 1;
    MDL.SensorCount = doc["MDL"]["SensorCount"] | 1;

    CFG.MeterCal = doc["FLOW"]["meterCal"] | 50.0f;
    CFG.Is3Wire = doc["FLOW"]["is3Wire"] | true;
    CFG.InvertRelay = doc["FLOW"]["invertRelay"] | false;
    CFG.InvertMotor = doc["FLOW"]["invertMotor"] | false;
    CFG.MasterPin = doc["FLOW"]["masterPin"] | 14;
    CFG.FlowPin = doc["FLOW"]["flowPin"] | 17;
    CFG.RegPinA = doc["FLOW"]["regPinA"] | 32;
    CFG.RegPinB = doc["FLOW"]["regPinB"] | 33;

    // Override per-sección del modo electroválvula. Si la clave no existe en
    // el JSON, todos quedan en -1 (usar global) por el SetDefault() previo.
    JsonArray secMode = doc["FLOW"]["sectionIs3Wire"];
    if (!secMode.isNull())
    {
        for (int i = 0; i < 10; i++)
            CFG.SectionIs3Wire[i] = (int8_t)(secMode[i] | -1);
    }

    CFG.pid.kp = doc["FLOW"]["pid"]["kp"] | 2.5f;
    CFG.pid.ki = doc["FLOW"]["pid"]["ki"] | 1.5f;
    CFG.pid.pwmMin = doc["FLOW"]["pid"]["pwmMin"] | 800;
    CFG.pid.pwmMinNeg = doc["FLOW"]["pid"]["pwmMinNeg"] | 800;

    JsonArray sections = doc["FLOW"]["sections"];
    for (int i = 0; i < 10; i++)
    {
        CFG.SectionPins[i][0] = sections[i][0] | NC;
        CFG.SectionPins[i][1] = sections[i][1] | NC;
    }

    // Sincronizar los canales de Sensor[] con lo que acaba de cargarse en CFG.
    // (LoadData no parsea el array "Sensors" del JSON — limitación pre-existente.
    //  Mientras tanto, propagamos los gains/meterCal globales a cada canal.)
    for (int i = 0; i < MaxProductCount; i++)
    {
        Sensor[i].Kp = CFG.pid.kp;
        Sensor[i].Ki = CFG.pid.ki;
        Sensor[i].Kd = CFG.pid.kd;
        Sensor[i].MinPWM = CFG.pid.pwmMin;
        Sensor[i].MinPWMNeg = CFG.pid.pwmMinNeg;
        Sensor[i].MeterCal = CFG.MeterCal;
    }
}

void SetDefault()
{
    CFG.FlowPin = 17;
    CFG.RegPinA = 32;
    CFG.RegPinB = 33;
    CFG.MasterPin = 14;

    CFG.SectionPins[0][0] = 27;
    CFG.SectionPins[0][1] = 12;
    CFG.SectionPins[1][0] = 13;
    CFG.SectionPins[1][1] = 15;
    CFG.SectionPins[2][0] = 2;
    CFG.SectionPins[2][1] = 4;

    CFG.MeterCal = 50.0f;
    CFG.Is3Wire = true;
    CFG.InvertRelay = false;
    CFG.InvertMotor = false;
    CFG.pid.pwmMin = 800;
    CFG.pid.pwmMinNeg = 800;
    CFG.pid.kp = 2.5f;
    CFG.pid.ki = 1.5f;
    CFG.pid.kd = 0.0f;

    // Override per-sección del modo de electroválvula: -1 = usar Is3Wire global.
    for (int i = 0; i < 10; i++)
        CFG.SectionIs3Wire[i] = -1;

    MDL.ID = 1;
    MDL.SensorCount = 1;
    MDL.RelayControl = 1;
    MDL.WorkPin = NC;
    MDL.WorkPinIsMomentary = false;

    // Defaults razonables para los canales de PID (los usa PID.cpp; sin esto
    // arrancan en 0 y el lazo nunca actúa).
    for (int i = 0; i < MaxProductCount; i++)
    {
        Sensor[i].MinPWM = 0;
        Sensor[i].MinPWMNeg = 0;
        Sensor[i].MaxPWM = 4095;
        Sensor[i].MaxIntegral = 4095;
        Sensor[i].PIDtime = 50;
        Sensor[i].PulseSampleSize = 5;
        Sensor[i].Kp = CFG.pid.kp;
        Sensor[i].Ki = CFG.pid.ki;
        Sensor[i].Kd = CFG.pid.kd;
        Sensor[i].MeterCal = CFG.MeterCal;
        Sensor[i].FlowEnabled = false;
        Sensor[i].AutoOn = true;
        Sensor[i].CalibActive = false;
        Sensor[i].TotalPulses = 0;
        Sensor[i].PidState = 0; // off

        // CommTime arranca = millis() (≈0 al boot) — sin esto, el timeout de
        // seguridad de 4s en CheckRelays() se dispara apenas el firmware
        // arranca y cierra las válvulas antes de que MQTT pueda conectar,
        // dando un nodo aparentemente "muerto" que no responde a comandos.
        Sensor[i].CommTime = millis();
    }
}

void SaveNetworks()
{
    StaticJsonDocument<512> doc;
    doc["SSID"] = MDLnetwork.SSID;
    doc["Password"] = MDLnetwork.Password;
    doc["Mode"] = MDLnetwork.WifiModeUseStation;
    doc["BrokerHost"] = MDLnetwork.BrokerHost;
    doc["BrokerPort"] = MDLnetwork.BrokerPort;
    File file = LittleFS.open(NETWORK_FILE, "w");
    serializeJson(doc, file);
    file.close();
}

void LoadNetworks()
{
    // Defaults — broker apunta a AgIO en la LAN del tractor.
    strlcpy(MDLnetwork.BrokerHost, "192.168.5.10", sizeof(MDLnetwork.BrokerHost));
    MDLnetwork.BrokerPort = 1883;
    MDLnetwork.WifiModeUseStation = true;

    if (!LittleFS.exists(NETWORK_FILE))
        return;
    File file = LittleFS.open(NETWORK_FILE, "r");
    StaticJsonDocument<512> doc;
    deserializeJson(doc, file);
    file.close();
    strlcpy(MDLnetwork.SSID, doc["SSID"] | "", sizeof(MDLnetwork.SSID));
    strlcpy(MDLnetwork.Password, doc["Password"] | "", sizeof(MDLnetwork.Password));
    MDLnetwork.WifiModeUseStation = doc["Mode"] | true;
    strlcpy(MDLnetwork.BrokerHost,
            doc["BrokerHost"] | "192.168.5.10",
            sizeof(MDLnetwork.BrokerHost));
    MDLnetwork.BrokerPort = doc["BrokerPort"] | 1883;
}

bool CheckPins() { return true; }