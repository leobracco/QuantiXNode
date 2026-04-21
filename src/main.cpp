#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "Structs.h"
#include "Globals.h"
#include "Constants.h"
#include "Log.h"
#include "core/Median.h"

// --- OBJETOS GLOBALES ---
ModuleConfig MDL;
SensorConfig Sensor[MaxProductCount];
ModuleNetwork MDLnetwork;

Adafruit_PWMServoDriver PWMServoDriver = Adafruit_PWMServoDriver();
PCF8574 PCF(0x20);
WebServer server(80);

// --- VARIABLES DE ESTADO ---
bool RelayLo = false;
bool RelayHi = false;
bool WifiMasterOn = false;
uint32_t WifiSwitchesTimer = 0;
bool Button[16];
bool GoodPins = false;
bool PCF_found = false;
bool ADSfound = false;
uint16_t PressureReading = 0;
uint32_t ResetTime = 0;

uint32_t LoopLast = 0;

bool WrkOn = false;
bool WrkLast = false;
bool Calibrando = false;
volatile uint32_t WheelCounts = 0;

extern void DoSetup();
extern void initMQTT();
extern void mqttLoop();

void setup()
{
    // Iniciar Serial primero para ver errores de arranque
    Serial.begin(115200);
    quantix::log::init(quantix::log::Level::INFO);  // nivel temporal hasta cargar config

    if (!LittleFS.begin(true))
    {
        LOG_E("fs", "Fallo al montar LittleFS");
    }

    DoSetup();

    // Configurar nivel de log definitivo a partir del config cargado.
    quantix::log::setLevel(static_cast<quantix::log::Level>(MDL.LogLevel));

    // Watchdog: se alimenta cada iteración de loop().
    esp_task_wdt_init(MDL.WatchdogSec, true);
    esp_task_wdt_add(NULL);

    initMQTT();
    LOG_I("boot", "QuantiX listo (MQTT)");
}

void loop()
{
    esp_task_wdt_reset();

    // 1. Manejo de clientes y comunicaciones (Siempre activos)
    server.handleClient();
    mqttLoop();

    // 2. Control de tiempo del bucle principal
    if (millis() - LoopLast >= quantix::constants::LOOP_INTERVAL_MS)
    {
        LoopLast = millis();

        // Obtener datos de sensores
        GetUPM();
        GetSpeed();

        // 3. Lógica de Motores
        for (int i = 0; i < MDL.SensorCount; i++)
        {
            if (Sensor[i].CalibrandoAhora == true)
            {
                Calibrando = true;
                SetPWM(i, Sensor[i].ManualAdjust);

                noInterrupts();
                uint32_t totalPulsesSnapshot = Sensor[i].TotalPulses;
                interrupts();

                if (millis() % 500 < 50)
                {
                    LOG_D("cal", "M%d: %lu / %lu (PWM=%d)",
                          i, (unsigned long)totalPulsesSnapshot,
                          (unsigned long)Sensor[i].PulsosMetaCalibracion,
                          Sensor[i].ManualAdjust);
                }

                if (totalPulsesSnapshot >= Sensor[i].PulsosMetaCalibracion)
                {
                    Calibrando = false;
                    Sensor[i].CalibrandoAhora = false;
                    Sensor[i].ManualAdjust = 0;
                    SetPWM(i, 0);
                    Sensor[i].AutoOn = true;

                    LOG_I("cal", "Meta alcanzada M%d pulsos=%lu",
                          i, (unsigned long)totalPulsesSnapshot);
                    sendMQTTStatus(i);
                }
            }
            else
            {
                // ELIMINADO: AdjustFlow(); <-- Esto rompía todo el I2C
                PIDmotor(i); // El PID ahora es autosuficiente
            }
        }

        // 4. Lógicas secundarias
        CheckRelays();

        ReadAnalog();
        SendComm();

        // Reinicio diferido (activado poniendo ResetTime = millis()).
        // Esperamos RESTART_DELAY_MS para que el cliente HTTP reciba la respuesta.
        if (ResetTime > 0 && (millis() - ResetTime > quantix::constants::RESTART_DELAY_MS))
        {
            ESP.restart();
        }
    }
}

// --- FUNCIONES AUXILIARES ---

bool WorkPinOn()
{
    if (MDL.WorkPin != NC)
    {
        bool WrkCurrent = !digitalRead(MDL.WorkPin);
        if (MDL.WorkPinIsMomentary)
        {
            if (WrkCurrent != WrkLast)
            {
                if (WrkCurrent)
                    WrkOn = !WrkOn;
                WrkLast = WrkCurrent;
            }
        }
        else
        {
            WrkOn = WrkCurrent;
        }
    }
    else
    {
        WrkOn = false;
    }
    return WrkOn;
}

uint32_t MedianFromArray(uint32_t buf[], int count)
{
    if (count <= 0) return 0;
    return quantix::core::medianFromArray(buf, (size_t)count);
}