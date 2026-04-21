#ifndef STRUCTS_H
#define STRUCTS_H

#include <Arduino.h>

// --- CONSTANTES GLOBALES ---
#define MaxSampleSize 20  // Tamaño máximo de muestras para el filtro de flujo
#define MaxProductCount 2
#define NC 0xFF
// --- ESTRUCTURA DE CONFIGURACIÓN DEL MÓDULO (MDL) ---
struct ModuleConfig {
    uint8_t ID;                 // ID del Módulo (para AgOpenGPS)
    uint8_t SensorCount;        // Cantidad de sensores/motores activos (1 o 2)
    
    // Configuración de Relés
    uint8_t RelayControl;       // 0=None, 1=GPIO, 5=PCA9685, 6=PCF8574
    uint8_t RelayControlPins[16]; // Mapeo de pines para relés (si es GPIO)
    bool InvertRelay;           // Invertir lógica de relés
    
    // Configuración de Flujo/Hardware
    bool InvertFlow;            // Invertir dirección del motor
    bool WorkPinIsMomentary;    // Si el switch de trabajo es pulsador o interruptor
    bool Is3Wire;               // Tipo de válvula (3 hilos vs 2 hilos)
    bool ADS1115Enabled;        // Si hay sensor de presión I2C
    uint8_t WorkPin;            // Pin del switch de trabajo (Input)

    // Seguridad WiFi
    char APpassword[12];        // Contraseña del Punto de Acceso (Hotspot)

    // --- Configuración operacional (añadido en refactor) ---
    uint8_t LogLevel;           // 0=ERROR 1=WARN 2=INFO 3=DEBUG (default 2)
    char HttpUser[16];          // Usuario HTTP Basic Auth para el portal
    char HttpPass[24];          // Password HTTP Basic Auth
    uint16_t WatchdogSec;       // Timeout del task watchdog (segundos, default 5)
    uint16_t CommTimeoutMs;     // Timeout sin coms antes de apagar relés (default 4000)
    uint16_t WifiTimeoutMs;     // Ventana de control web tras toggle (default 30000)
};

// --- ESTRUCTURA DE CONFIGURACIÓN DE SENSOR/MOTOR (Sensor[]) ---
struct SensorConfig {
    // Pines de Hardware
    uint8_t FlowPin;            // Entrada de pulsos (Sensor Flujo)
    uint8_t DirPin;             // Salida Dirección (Para drivers simples)
    uint8_t PWMPin;             // Salida PWM (Para drivers simples)
    
    // Pines específicos ESP32 (Puente H)
    uint8_t IN1;                // Pin Motor A
    uint8_t IN2;                // Pin Motor B
    //Calibracion
    // --- NUEVO: RPM y Calibración ---
    float RPM;                  // RPM actuales
    uint16_t PulsesPerRev;      // Pulsos por vuelta (para calcular RPM real)
    
    volatile bool CalibActive;  // ¿Calibrando? (modificado desde callback MQTT)
    uint32_t CalibTargetPulses; // Meta de pulsos (ej: 240)
    // --------------------------------
    // Estado y Control
    bool FlowEnabled;           // Si la sección/motor está activo
    bool AutoOn;                // Modo Automático vs Manual
    int ManualAdjust;           // Valor de PWM manual
    uint8_t ControlType;        // 0=Valve, 1=Close, 2=Motor, 3=Fan
    
    // Valores de Proceso
    float UPM;                  // Unidades Por Minuto (Flow Rate Actual)
    float TargetUPM;            // Setpoint (Objetivo)
    float PWM;                  // Salida actual calculada (0-255)
    float Hz;                   // Frecuencia actual de pulsos
    volatile uint32_t TotalPulses; // Acumulador para volumen (incrementado en GetUPM, leído en loop)
    float MeterCal;             // Calibración (Pulsos por Unidad)

    // --- PARÁMETROS PID AVANZADO ---
    float Kp;
    float Ki;
    float Kd;
    uint32_t PIDtime;           // Intervalo de ejecución del PID (ms)
    
    // Límites de Salida
    uint16_t MinPWM;  // <--- CAMBIADO de uint8_t a uint16_t
    uint16_t MaxPWM;  // <--- CAMBIADO de uint8_t a uint16_t
    float MaxIntegral;        // Límite del término integral (Anti-Windup)

    // Lógica SK21 / Custom
    uint8_t Deadband;           // Zona muerta (%)
    uint8_t BrakePoint;         // Punto de frenado (%)
    uint8_t SlewRate;           // Rampa máxima de cambio por ciclo
    
    // Lógica de Válvulas Temporizadas (Combo)
    float TimedMinStart;
    uint32_t TimedPause;
    
    // Configuración de Lectura de Pulsos (Rate.cpp)
    uint32_t PulseMin;          // Filtro mínimo (us)
    uint32_t PulseMax;          // Filtro máximo (us)
    uint8_t PulseSampleSize;    // Tamaño del buffer de promedio
    
    // Comunicación
    volatile uint32_t CommTime; // Timestamp del último paquete recibido (escrito desde callback MQTT)
};

// --- ESTRUCTURA DE RED (MDLnetwork) ---
struct ModuleNetwork {
    uint8_t IP0;
    uint8_t IP1;
    uint8_t IP2;
    uint8_t IP3;
    
    char SSID[24];              // Nombre de la red WiFi a conectar
    char Password[24];          // Contraseña de la red WiFi

    bool WifiModeUseStation;    // true = Conectar a Router, false = Crear Hotspot
    uint16_t Identifier;        // Número mágico para validar EEPROM (9876)

    // --- MQTT (añadido en refactor: antes estaba hardcodeado) ---
    char MqttHost[40];          // IP o hostname del broker MQTT
    uint16_t MqttPort;          // Puerto MQTT (default 1883)
    char MqttUser[24];          // Usuario MQTT (vacío = sin auth)
    char MqttPass[24];          // Password MQTT
    uint16_t MqttKeepAlive;     // Keep-alive en segundos (default 15)
};

#endif