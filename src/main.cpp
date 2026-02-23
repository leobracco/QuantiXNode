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
const uint32_t LoopTime = 50; 

bool WrkOn = false;
bool WrkLast = false;
volatile uint32_t WheelCounts = 0;

extern void DoSetup();      
extern void initMQTT();     
extern void mqttLoop();     

void setup()
{
    // Iniciar Serial primero para ver errores de arranque
    Serial.begin(115200); 

    if(!LittleFS.begin(true)){
        Serial.println("Fallo al montar LittleFS");
    }

    DoSetup(); 
    initMQTT();
    Serial.println(F("--- QUANTIX ARRANCADO (SOLO MQTT) ---"));
}

void loop()
{
    server.handleClient();
    
    // Solo bucle MQTT
    mqttLoop(); 
    void CheckCalibration();
    //CheckCalibration(); // Chequeo rápido
    if (millis() - LoopLast >= LoopTime)
    {
        LoopLast = millis();

        CheckRelays();
        GetUPM(); 
        GetSpeed();

        for (int i = 0; i < MDL.SensorCount; i++)
        {
            PIDmotor(i); 
        }

        AdjustFlow();
        ReadAnalog();
        
        SendComm(); 

        if (ResetTime > 0)
        {
            if (millis() - ResetTime > 5000) ESP.restart();
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
                if (WrkCurrent) WrkOn = !WrkOn; 
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
        for (int i = 0; i < count; i++) sorted[i] = buf[i];

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

        if (count % 2 == 1) Result = sorted[count / 2];
        else { int mid = count / 2; Result = (sorted[mid - 1] + sorted[mid]) / 2; }
    }
    return Result;
}