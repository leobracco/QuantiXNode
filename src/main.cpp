#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include <ESP2SOTA.h>
#include <LittleFS.h>

#include "Structs.h"
#include "Globals.h"
#include "PCA95x5_RC.h"

// --- OBJETOS GLOBALES ---
ModuleConfig MDL;
SensorConfig Sensor[MaxProductCount];
ModuleNetwork MDLnetwork;

// RC15 PCB: PCA9685 address 0x55 (A5-A0 = 1010101)
Adafruit_PWMServoDriver PWMServoDriver = Adafruit_PWMServoDriver(0x55);
PCF8574 PCF(0x20);
WebServer server(80);

// --- VARIABLES DE ESTADO ---
uint8_t RelayLo = 0;
uint8_t RelayHi = 0;
bool WifiMasterOn = false;
uint32_t WifiSwitchesTimer = 0;
bool Button[16];
bool GoodPins = false;
bool PCF_found = false;
bool ADSfound = false;
uint16_t PressureReading = 0;
uint32_t ResetTime = 0;

uint32_t LoopLast = 0;
const uint32_t LoopTime = 50;

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

    if (!LittleFS.begin(true))
    {
        Serial.println("Fallo al montar LittleFS");
    }

    DoSetup();
    initMQTT();
    Serial.println(F("--- QUANTIX ARRANCADO (SOLO MQTT) ---"));
}

void loop()
{
    // 1. Manejo de clientes y comunicaciones (Siempre activos)
    server.handleClient();
    mqttLoop();

    // 2. Control de tiempo del bucle principal (50ms)
    if (millis() - LoopLast >= LoopTime)
    {
        LoopLast = millis();

        // Obtener datos de sensores
        GetUPM();
        GetSpeed();

        // 3. Lógica de Motores
        for (int i = 0; i < MDL.SensorCount; i++)
        {
            if (Sensor[i].CalibActive == true)
            {
                Calibrando = true;
                SetPWM(i, Sensor[i].ManualAdjust);

                if (millis() % 500 < 50)
                {
                    Serial.printf("🧪 Calibrando M%d: %ld / %ld (PWM: %d)\n",
                                  i, Sensor[i].TotalPulses, Sensor[i].CalibTargetPulses, Sensor[i].ManualAdjust);
                }

                if (Sensor[i].TotalPulses >= Sensor[i].CalibTargetPulses)
                {
                    Calibrando = false;
                    Sensor[i].CalibActive = false;
                    Sensor[i].ManualAdjust = 0;
                    SetPWM(i, 0);
                    Sensor[i].AutoOn = true;

                    Serial.printf("🏁 META ALCANZADA: %ld pulsos.\n", Sensor[i].TotalPulses);
                    sendMQTTStatus(i);
                }
            }
            else
            {
                // ELIMINADO: AdjustFlow(); <-- Esto rompía todo el I2C
                PIDmotor(i); // El PID ahora es autosuficiente
            }
        }

        // Diagnóstico cada 2 seg
        static uint32_t lastDiag = 0;
        if (millis() - lastDiag >= 2000)
        {
            lastDiag = millis();
            for (int d = 0; d < MDL.SensorCount; d++)
            {
                Serial.printf("[DIAG M%d] ISR:%lu Filtrados:%lu Hz:%.1f RPM:%.0f PWM:%.0f PulseMin:%lu PPR:%d\n",
                    d, TotalInterrupts[d], Sensor[d].TotalPulses,
                    Sensor[d].Hz, Sensor[d].RPM, Sensor[d].PWM,
                    Sensor[d].PulseMin, Sensor[d].PulsesPerRev);
            }
        }

        // 4. Lógicas secundarias
        CheckRelays();

        ReadAnalog();
        SendComm();

        // Manejo de Reinicio pendiente
        if (ResetTime > 0 && (millis() - ResetTime > 5000))
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
    uint32_t Result = 0;
    if (count > 0)
    {
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

        if (count % 2 == 1)
            Result = sorted[count / 2];
        else
        {
            int mid = count / 2;
            Result = (sorted[mid - 1] + sorted[mid]) / 2;
        }
    }
    return Result;
}