#ifndef STRUCTS_H
#define STRUCTS_H

#include <Arduino.h>

// --- CONSTANTES GLOBALES ---
#define MaxSampleSize 20
#define MaxProductCount 2
#define NC 0xFF

// --- TIPO DE MOTOR ---
enum MotorType_t : uint8_t {
    MOTOR_ELECTRIC = 0,
    MOTOR_HYDRAULIC = 1
};

// --- ESTRUCTURA DE CONFIGURACIÓN DEL MÓDULO (MDL) ---
struct ModuleConfig {
    uint8_t ID;
    uint8_t SensorCount;

    // Relés
    uint8_t RelayControl;       // 0=None, 1=GPIO, 5=PCA9685, 6=PCF8574
    uint8_t RelayControlPins[16];
    bool InvertRelay;

    // Hardware
    bool InvertFlow;
    bool WorkPinIsMomentary;
    bool Is3Wire;
    bool ADS1115Enabled;
    uint8_t WorkPin;

    // WiFi
    char APpassword[12];
};

// --- ESTRUCTURA DE CONFIGURACIÓN DE SENSOR/MOTOR ---
struct SensorConfig {
    // Pines de Hardware
    uint8_t FlowPin;
    uint8_t FlowPinB;    // Segundo sensor (NC = deshabilitado, si presente promedia con FlowPin)
    uint8_t DirPin;
    uint8_t PWMPin;
    uint8_t IN1;
    uint8_t IN2;

    // Tipo de actuador
    MotorType_t MotorType;      // MOTOR_ELECTRIC o MOTOR_HYDRAULIC

    // RPM y Calibración
    float RPM;
    uint16_t PulsesPerRev;
    bool CalibActive;
    uint32_t CalibTargetPulses;

    // Estado y Control
    bool FlowEnabled;
    bool AutoOn;
    int ManualAdjust;
    uint8_t ControlType;

    // Valores de Proceso
    float UPM;                  // Hz actual (Pulsos/seg)
    float TargetUPM;            // Hz objetivo
    float PWM;                  // Salida PWM actual (0-4095)
    float Hz;                   // Frecuencia actual de pulsos
    uint32_t TotalPulses;
    float MeterCal;

    // --- PARÁMETROS PID ---
    float Kp;
    float Ki;
    float Kd;
    uint32_t PIDtime;           // Intervalo PID (ms). Eléctrico: 50, Hidráulico: 200+

    // Límites de Salida
    uint16_t MinPWM;
    uint16_t MaxPWM;
    float MaxIntegral;

    // Feedforward
    float MaxHz;                // Hz medidos a PWM máximo (calibración)
    float FFGain;               // Ganancia feedforward (default 1.0)

    // Filtro sensor
    float Alpha;                // Coeficiente EMA: eléctrico 0.4, hidráulico 0.2

    // Slew rate (PWM/segundo, independiente de PIDtime)
    float SlewRatePerSec;       // Eléctrico: 5000, Hidráulico: 1000
    float TargetSlewHzPerSec;   // Rampa de target Hz/seg (default 50)

    // Zona muerta y freno
    uint8_t Deadband;           // % zona muerta
    uint8_t BrakePoint;

    // Hidráulico
    float HydDeadZonePWM;       // PWM donde la válvula empieza a abrir
    float HydHysteresis;        // % histéresis del carrete (default 8)
    uint16_t HydPWMFreq;        // Frecuencia PWM solenoide (default 150 Hz)

    // Legacy (compatibilidad)
    uint8_t SlewRate;           // Viejo slew por tick — se migra a SlewRatePerSec

    // Lectura de Pulsos
    uint32_t PulseMin;
    uint32_t PulseMax;
    uint8_t PulseSampleSize;

    // Temporizados
    float TimedMinStart;
    uint32_t TimedPause;

    // Comunicación
    uint32_t CommTime;
};

// --- ESTRUCTURA DE RED ---
struct ModuleNetwork {
    uint8_t IP0;
    uint8_t IP1;
    uint8_t IP2;
    uint8_t IP3;

    char SSID[24];
    char Password[24];

    bool WifiModeUseStation;
    uint16_t Identifier;
};

#endif
