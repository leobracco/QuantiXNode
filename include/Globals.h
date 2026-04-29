#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include "Structs.h"
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include <WebServer.h>

#define FW_VERSION "2.1.0"

extern const uint16_t InoID;
extern const uint8_t InoType;


extern ModuleConfig MDL;
extern SensorConfig Sensor[];
extern ModuleNetwork MDLnetwork;

extern Adafruit_PWMServoDriver PWMServoDriver;
extern PCF8574 PCF;
extern WebServer server;

// VARIABLES DE ESTADO
extern uint8_t RelayLo;
extern uint8_t RelayHi;
extern bool WifiMasterOn;
extern uint32_t WifiSwitchesTimer;
extern bool Button[16]; 
extern bool GoodPins;
extern bool PCF_found;  
extern bool ADSfound;   
extern uint16_t PressureReading;
extern uint32_t ResetTime;
extern volatile uint32_t WheelCounts; 

// PROTOTIPOS
void DoSetup();
void SaveData();
void LoadData();
void SaveNetworks();
void LoadNetworks();
bool CheckPins();
// Eliminado CRC y GoodCRC

extern bool HasSensorB[];
void IRAM_ATTR ISR_Sensor0B();
void IRAM_ATTR ISR_Sensor1B();

void PIDmotor(byte ID);
void AutoTuneStart(byte ID);
void AutoTuneStop(byte ID);
void AutoTuneTick(byte ID);
bool AutoTuneActive(byte ID);
void AdjustFlow();
void SetPWM(byte ID, float pwmVal = 0); 
void GetUPM();
void CachePulseFilter();
void ResetPulseCounters(byte ID);      
void GetSpeed();   
void ResetPIDState(byte ID); 
void IRAM_ATTR ISR_Sensor0();
void IRAM_ATTR ISR_Sensor1();
extern volatile uint32_t TotalInterrupts[MaxProductCount];
void SendComm();
void initMQTT();
void CheckCalibration();
void sendMQTTStatus(byte ID);
void mqttLoop();

// Interfaz Web
String GetPage0();
String GetPage1();
String GetPage2();

void CheckRelays();
void ReadAnalog();

bool WorkPinOn();
uint32_t MedianFromArray(uint32_t buf[], int count);

#endif