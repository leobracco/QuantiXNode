#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include <LittleFS.h>
#include "esp_task_wdt.h"
#include "esp_system.h"

#include "Structs.h"
#include "Globals.h"
#include "AgpSafeMode.h"  // Tanda 2: contador de crashes en NVS

// ── Task Watchdog Timer (TWDT) ──
// Red de seguridad por hardware: si loop() no llama a wdtFeed() durante
// WDT_TIMEOUT_SEC, el ESP32 reinicia solo. Cubre cuelgues por LittleFS lock,
// MQTT half-open patológico, callbacks infinitos, AP softAP reentrante.
// Timeout generoso (30s) para no falsificar con el wait STA de 8s al boot —
// esos bloques alimentan WDT vía wdtFeed().
// WDT corto = recuperación rápida ante cuelgue. A 8 km/h en campo:
//   30s → 67 m de hang antes de reset
//    8s → 17.7 m antes de reset (acotado por radio de aplicación aceptable)
// Todos los waits sincrónicos largos (WiFi.begin 15s, OTA stream) tienen
// wdtFeed() explícito o están bracketeados con wdtSuspend()/wdtResume().
#define WDT_TIMEOUT_SEC 8

static void wdtInit()
{
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
    esp_task_wdt_add(NULL);
    Serial.printf("🐶 WDT activo: timeout=%ds  reset_reason=%d\n",
                  WDT_TIMEOUT_SEC, (int)esp_reset_reason());
}

void wdtFeed()    { esp_task_wdt_reset(); }
void wdtSuspend() { esp_task_wdt_delete(NULL); } // antes de OTA / flash largo
void wdtResume()  { esp_task_wdt_add(NULL); }

// Devuelve nombre amigable del último reset reason. Se publica en el
// announcement MQTT para que el panel de PilotX pueda mostrar por qué
// se reinició el nodo (cuelgue WDT, brownout, panic) sin USB serial.
const char* bootReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext_reset";
        case ESP_RST_SW:        return "sw_reset";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// --- 1. OBJETOS DE HARDWARE ---
Adafruit_PWMServoDriver PWMServoDriver = Adafruit_PWMServoDriver(0x40);
PCF8574 PCF(0x20);
WebServer server(80);

// --- 2. DEFINICIÓN DE VARIABLES GLOBALES (Reservando memoria) ---
// Estas variables son las que los otros archivos buscan como 'extern'
ModuleConfig MDL;
SensorConfig Sensor[MaxProductCount];
ModuleNetwork MDLnetwork;
FlowConfig CFG;

const uint16_t InoID = 123; // ID único del firmware
uint16_t seccionesBits = 0;
// Mask de override manual desde el portal web — bit seteado = sección forzada
// APAGADA. processValves() en Relays.cpp lo aplica antes de tocar el hardware.
uint16_t sectionForceOff = 0;
float caudalReal = 0.0f;
double outputPWM = 0.0;
volatile unsigned long pulsosCaudal = 0;

// PID y Control
uint32_t LastCheck[MaxProductCount];
float LastPWM[MaxProductCount] = {0};
float IntegralSum[MaxProductCount];

// Cálculo de tasa (Rate)
uint32_t LastPulse[MaxProductCount];
uint32_t ReadLast[MaxProductCount];
uint32_t PulseTime[MaxProductCount];
volatile uint32_t Samples[MaxProductCount][MaxSampleSize];
volatile uint16_t PulseCount[MaxProductCount];
volatile uint8_t SamplesCount[MaxProductCount];
volatile uint8_t SamplesIndex[MaxProductCount];

// Estados del sistema (CRÍTICO: Aquí definimos GoodPins)
bool RelayLo = false;
bool RelayHi = false;
bool WifiMasterOn = false;
uint32_t WifiSwitchesTimer = 0;
bool Button[16];
bool GoodPins = false; // <--- DEFINICIÓN QUE FALTABA
bool PCF_found = false;
bool ADSfound = false;
uint16_t PressureReading = 0;
uint32_t ResetTime = 0;
volatile uint32_t WheelCounts = 0;

uint32_t LoopLast = 0;
const uint32_t LoopTime = 50;

bool WrkOn = false;
bool WrkLast = false;
bool Calibrando = false;

// --- 3. PROTOTIPOS EXTERNOS ---
extern void DoSetup();
extern void initMQTT();
extern void mqttLoop();
extern void GetUPM();
extern void GetSpeed();
extern void PIDmotor(byte ID);
extern void CheckRelays();
extern void ReadAnalog();
extern void SendComm();
extern void SetPWM(byte ID, float pwmVal);
extern void sendMQTTStatus(byte ID);
extern void RunCalibAuto(byte ID);
extern bool IsAutoTuneActive(byte ID);
extern void CalibStop(byte ID, bool cancelled);

// OTA — encolada por el callback MQTT, ejecutada en loop().
extern volatile bool otaPendiente;

void setup()
{
    Serial.begin(115200);

    // WDT primero — `DoSetup()` contiene wait STA con delay(500)×N que alimenta
    // WDT explícitamente vía wdtFeed() para no falsificar.
    wdtInit();

    // Tanda 2: safe-mode counter. Si este boot fue por crash, incrementa
    // crash_count en NVS; tras 3 consecutivos → safe_mode activo (handler
    // MQTT rechaza OTA/calibración/autotune hasta clear_safe_mode).
    int cc = AgpSafeMode_begin();
    Serial.printf("🛡 SafeMode: crash_count=%d active=%s\n",
                  cc, AgpSafeMode_isActive() ? "YES" : "no");

    // LittleFS.begin(false) → NO formatear automáticamente si el mount falla.
    // El true por defecto borraba toda la config (calibración, modos cable,
    // network.json) cuando había una sola página corrupta tras un brownout,
    // dejando al nodo "como nuevo" en el campo y sin posibilidad de recuperar
    // la calibración. Si falla el primer mount, hacemos UN segundo intento
    // tras un pequeño delay (filesystem aún montándose) y sólo formateamos
    // como último recurso, dejando log explícito para que se vea en el portal.
    //
    // Con WDT=8s, el formato puede ser bloqueante varios segundos → suspender
    // WDT alrededor del bloque para no resetear en bucle infinito de boot.
    if (!LittleFS.begin(false))
    {
        Serial.println("⚠ LittleFS mount falló — reintentando en 200ms");
        delay(200);
        wdtFeed();
        if (!LittleFS.begin(false))
        {
            Serial.println("⛔ LittleFS irrecuperable — FORMATEANDO (se pierde config)");
            wdtSuspend();
            bool fmtOk = LittleFS.begin(true);
            wdtResume();
            if (!fmtOk)
                Serial.println("💥 Falló incluso con formato — continuando sin FS");
        }
        else
        {
            Serial.println("✅ LittleFS montado en segundo intento");
        }
    }
    wdtFeed();

    DoSetup(); // Llama a la configuración en Begin.cpp
    wdtFeed();
    Serial.println(F("--- FLOWX NODE ONLINE ---"));
}

void loop()
{
    // Alimentar WDT en cada iteración del loop principal.
    wdtFeed();

    server.handleClient();
    mqttLoop();

    // OTA encolada por el callback MQTT — se procesa fuera del callback porque
    // handleOTACommand() es bloqueante (descarga ~1MB + flash + reboot).
    if (otaPendiente) procesarOtaPendiente();

    // SaveConfig diferido — escritura LittleFS fuera del callback MQTT.
    processPendingSave();

    if (millis() - LoopLast >= LoopTime)
    {
        LoopLast = millis();

        GetUPM();
        GetSpeed();

        for (int i = 0; i < MDL.SensorCount; i++)
        {
            if (IsManualActive(i))
            {
                // Modo manual (búsqueda de pwm_min set/observar). Toma control
                // exclusivo del PWM; PID/calibración/caracterización no corren.
                // Failsafe de comms-loss y timeout duro adentro de RunManual().
                RunManual(i);
            }
            else if (IsCharActive(i))
            {
                // Caracterización (rampa PWM 0..4095 buscando pwm_min/hz_max).
                // Toma control del PWM — el PID y la calibración no corren.
                RunChar(i);
            }
            else if (IsAutoTuneActive(i))
            {
                // El AutoTune toma control total del PWM; el PID no corre.
                RunCalibAuto(i);
            }
            else if (Sensor[i].CalibActive)
            {
                // Calibración manual: aplicamos ManualAdjust al actuador.
                // Si se setea CalibTargetPulses > 0, mantenemos el auto-stop
                // legacy por cantidad de pulsos. CalibStart() (CalibAuto.cpp)
                // lo deja en 0 para que SÓLO termine por calibrar_stop.
                SetPWM(i, Sensor[i].ManualAdjust);
                // Espejar el PWM realmente aplicado en Sensor.PWM: sendMQTTStatus
                // publica ESE campo. Sin esto la telemetría/widget mostraba el
                // último valor que dejó el PID (congelado) mientras el motor
                // giraba de verdad al PWM de calibración.
                Sensor[i].PWM = Sensor[i].ManualAdjust;
                if (Sensor[i].CalibTargetPulses > 0 &&
                    Sensor[i].TotalPulses >= (uint32_t)Sensor[i].CalibTargetPulses)
                {
                    CalibStop(i, false);
                }
                else
                {
                    RunCalibAuto(i); // gestiona timeout de seguridad
                }
            }
            else
            {
                PIDmotor(i);
            }
        }

        CheckRelays();
        ReadAnalog();
        SendComm();

        if (ResetTime > 0 && (millis() - ResetTime > 5000))
            ESP.restart();
    }
}

// Funciones auxiliares
bool WorkPinOn()
{
    if (MDL.WorkPin == NC)
        return false;
    bool current = !digitalRead(MDL.WorkPin);
    if (MDL.WorkPinIsMomentary)
    {
        if (current != WrkLast)
        {
            if (current)
                WrkOn = !WrkOn;
            WrkLast = current;
        }
    }
    else
    {
        WrkOn = current;
    }
    return WrkOn;
}

uint32_t MedianFromArray(uint32_t buf[], int count)
{
    if (count <= 0)
        return 0;
    uint32_t sorted[20];
    for (int i = 0; i < count; i++)
        sorted[i] = buf[i];
    for (int i = 1; i < count; i++)
    {
        uint32_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return (count % 2 == 1) ? sorted[count / 2] : (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
}