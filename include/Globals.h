#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <WebServer.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include "Structs.h"

#define FW_VERSION "1.9.3" // Buffer MQTT 1024->2048 (drops silenciosos de config grande); 1.9.2: Fix deteccion PWM: congelar secciones mientras hay deteccion activa (manual/caracterizar/autotune/calibrar) para que el sec=0 del bridge con tractor parado no aborte el barrido; 1.9.1: Modo manual MQTT (buscar pwm_min set/observar) + pwm_min bidireccional (piso PID por sentido abrir/cerrar); 1.8.1: Banda proporcional: velocidad motor mapea error->[pwm_min,pwm_max]; 1.8.0: valvula 3-posiciones sin integral + invertMotor; 1.7.0: PID bidireccional; 1.6.x: caracterizar/telemetria

// --- VARIABLES GLOBALES (Externas) ---
extern ModuleConfig MDL;
extern FlowConfig CFG;
extern SensorConfig Sensor[MaxProductCount];
extern ModuleNetwork MDLnetwork;
extern WebServer server;
extern Adafruit_PWMServoDriver PWMServoDriver;
extern PCF8574 PCF;

extern uint16_t seccionesBits;
extern uint16_t sectionForceOff; // mask de override manual (portal web)
extern float caudalReal;
extern double outputPWM;
extern volatile unsigned long pulsosCaudal;
extern bool RelayLo, RelayHi, WifiMasterOn, GoodPins, PCF_found;
extern uint32_t WifiSwitchesTimer, ResetTime;
extern bool Button[16];

// --- PROTOTIPOS DE FUNCIONES (¡Esto quita los errores de compilación!) ---

extern uint32_t LastCheck[MaxProductCount];
extern float LastPWM[MaxProductCount];
extern float IntegralSum[MaxProductCount];

extern uint32_t LastPulse[MaxProductCount];
extern uint32_t ReadLast[MaxProductCount];
extern uint32_t PulseTime[MaxProductCount];
extern volatile uint32_t Samples[MaxProductCount][MaxSampleSize];
extern volatile uint16_t PulseCount[MaxProductCount];
extern volatile uint8_t SamplesCount[MaxProductCount];
extern volatile uint8_t SamplesIndex[MaxProductCount];

// --- Prototipos ---

void ResetPIDState(byte ID);

// En Begin.cpp
void DoSetup();
void LoadData();
void SaveData();   // Requerido por GUI.cpp
void SaveConfig(); // Requerido por MQTT_Custom.cpp
void LoadNetworks();
void SaveNetworks(); // Requerido por GUI.cpp

// En Relays.cpp
void processValves(uint16_t bits); // Requerido por MQTT_Custom.cpp
void CheckRelays();

// En GUI.cpp / Pages.cpp — portal web (estilo Agro Parallel)
// Sólo 2 páginas: cortes manuales de secciones + configuración de red.
void HandleRoot();    // "/"   — GET=GetPage0 (cortes) / POST=handleCredentials
void HandlePage2();   // "/p2" — Red (WiFi + Broker)
String GetPage0();
String GetPage2();

// En MQTT_Custom.cpp
void initMQTT();
void mqttLoop();
void sendMQTTStatus(byte ID);
void publicarAnnouncement(bool retained);
void procesarOtaPendiente(); // ejecuta una OTA encolada por el callback MQTT
void processPendingSave();   // ejecuta un SaveConfig diferido (fuera del callback)

// Otros
void GetUPM();
void GetSpeed();
void PIDmotor(byte ID);
void SetPWM(byte ID, float pwmVal);
void ReadAnalog();
void SendComm();
uint32_t MedianFromArray(uint32_t buf[], int count);

// --- Calibración + Auto-Tune (CalibAuto.cpp) ---
// Lanzados desde mqttCallback() al recibir agp/flow/<uid>/cmd/<verb>.
// RunCalibAuto(ID) corre en cada tick del loop por canal: avanza la máquina
// de estados del auto-tune por relay feedback y publica el result al terminar.
void CalibStart(byte ID, float volL, int pwm);
void CalibStop(byte ID, bool cancelled);
void AutoTuneStart(byte ID, float setpointHz, int pwmHigh, int pwmLow);
void AutoTuneStop(byte ID, bool cancelled);
void RunCalibAuto(byte ID);
bool IsAutoTuneActive(byte ID);
bool IsCalibActive(byte ID);

// --- Caracterización motor (descubrir pwm_min real + hz_max) ---
// Rampa PWM 0 → 4095 con dwell por paso, mide Hz por punto y publica
// la curva + el primer PWM con flujo + el Hz máximo. Requiere secciones
// abiertas (master + al menos una sección) y bomba con agua circulando.
void CharStart(byte ID);
void CharStop(byte ID, bool cancelled);
void RunChar(byte ID);
bool IsCharActive(byte ID);

// --- Modo manual (búsqueda de pwm_min set/observar, bidireccional) ---
// El operario fija un PWM con signo (-4095..+4095) y observa el caudal en vivo
// por status_live. ManualSet toma control exclusivo del PWM (el PID no corre);
// RunManual lo re-aplica cada tick y corta por failsafe de comms-loss.
void ManualSet(byte ID, float value);
void ManualStop(byte ID);
void RunManual(byte ID);
bool IsManualActive(byte ID);

// Publicación de resultados (definidos en MQTT_Custom.cpp).
void publishCalibResult(byte ID, bool ok, uint32_t pulsos, uint32_t durationMs, const char *err);
void publishAutoTuneResult(byte ID, bool ok, float kp, float ki, float kd, float ku, float tuMs, const char *err);
void publishCharResult(byte ID, bool ok, int pwmMin, int pwmMinEstable,
                       float hzMax, float lminMax,
                       const int *pwmCurve, const float *hzCurve, int curveLen,
                       const char *err);

// ISRs
void IRAM_ATTR ISR_Sensor0();
void IRAM_ATTR ISR_Sensor1();

#endif