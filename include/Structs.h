#ifndef STRUCTS_H
#define STRUCTS_H

#include <Arduino.h>

const uint8_t NC = 255;
const uint8_t MaxProductCount = 2;
const uint8_t MaxSampleSize = 20;

struct FlowConfig
{
    float MeterCal;    // Pulsos por Litro
    float dosisTarget; // L/min objetivo enviado por el Bridge
    bool Is3Wire;      // Default GLOBAL: true=3 cables (on/off simple),
                       //                  false=2 cables (inversión de polaridad).
                       // Cada sección puede overridear esto con SectionIs3Wire[i].
    bool InvertRelay;  // Invertir lógica de activación (módulos relé activos-en-LOW)
    bool InvertMotor;  // Invertir el sentido del motor de la válvula: si el
                       // cableado hace que +PWM cierre en vez de abrir, esto
                       // intercambia los dos canales del puente H en SetPWM.

    struct
    {
        float kp, ki, kd;
        int pwmMin;    // 0-4095: piso de PWM al ABRIR (output PID > 0)
        int pwmMinNeg; // 0-4095: piso de PWM al CERRAR (output PID < 0)
    } pid;

    // Pines de Hardware (Estándar SK21)
    uint8_t FlowPin;            // Sensor de caudal principal
    uint8_t RegPinA;            // Pin A Motor Regulador
    uint8_t RegPinB;            // Pin B Motor Regulador
    uint8_t MasterPin;          // Salida de alta potencia
    uint8_t SectionPins[10][2]; // [Sección][PinA, PinB] para 2 cables

    // Override per-sección del modo de la electroválvula:
    //   -1 = usar Is3Wire global (default — la mayoría de instalaciones son
    //        homogéneas: o todas 2-cables o todas 3-cables)
    //    0 = forzar 2-cables (inversión de polaridad pinA/pinB)
    //    1 = forzar 3-cables (solo da positivo o nada en pinA)
    // Permite mezclar electroválvulas distintas en la misma barra.
    int8_t SectionIs3Wire[10];
};
// --- ENUM DE TIPOS DE CONTROL ---
enum ControlTypeEnum
{
    StandardValve_ct = 0,
    ComboClose_ct = 1,
    Motor_ct = 2,
    Fan_ct = 3,
    TimedCombo_ct = 4
};

struct ModuleConfig
{
    uint16_t ID;
    uint8_t SensorCount;
    uint8_t RelayControl; // 1: GPIO, 5: PCA9685, 6: PCF8574
    uint8_t RelayControlPins[16];
    uint8_t WorkPin;
    bool WorkPinIsMomentary;
    char APpassword[16];
    bool InvertRelay;
    uint8_t PressurePin;
    float WheelCal;
    uint8_t WheelSpeedPin;
};

struct SensorConfig
{
    uint8_t FlowPin;
    uint8_t IN1;
    uint8_t IN2;
    uint8_t DirPin;
    uint8_t PWMPin;

    float Kp, Ki, Kd;
    float PWM;
    int MinPWM, MaxPWM;
    int MinPWMNeg; // piso de PWM al cerrar (magnitud); MinPWM es el de abrir
    float MaxIntegral;
    uint32_t PIDtime;

    uint8_t Deadband;
    uint8_t BrakePoint;
    uint8_t SlewRate;
    float PIDslowAdjust;

    float MeterCal;
    float TargetUPM;
    float UPM;
    float Hz;
    float RPM;
    uint16_t PulsesPerRev;
    uint8_t PulseSampleSize;

    uint8_t ControlType; // <--- AGREGADO: Define el tipo de actuador

    bool FlowEnabled;
    bool AutoOn;
    int ManualAdjust;

    uint32_t TotalPulses;
    bool CalibActive;
    long CalibTargetPulses;
    uint32_t CommTime;

    // Estado del lazo PID reportado a AOG vía MQTT (status_live.pid_estado):
    //   0=off  1=sin_pulsos  2=saturado  3=ok
    // Lo escribe PID.cpp en cada cálculo. MQTT_Custom.cpp lo serializa a string.
    uint8_t PidState;
};

struct ModuleNetwork
{
    char SSID[32];
    char Password[32];
    bool WifiModeUseStation;

    // Broker MQTT en LAN (corre embebido en AgIO, ver CLAUDE.md raíz).
    // Default: 192.168.5.10:1883. Configurable desde el portal web del nodo
    // y persistido en /network.json. Si BrokerHost queda vacío se usa el default.
    char BrokerHost[32];
    uint16_t BrokerPort;
};

#endif