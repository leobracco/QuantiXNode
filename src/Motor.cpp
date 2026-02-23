#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"

// Definiciones de tipos de control (por si no están en otro lado)
#ifndef StandardValve_ct
    #define StandardValve_ct 0
    #define ComboClose_ct    1
    #define Motor_ct         2
    #define Fan_ct           3
    #define TimedCombo_ct    4
#endif

// Función para ajustar el flujo general (Llamada desde main)
void AdjustFlow()
{
    for (int i = 0; i < MDL.SensorCount; i++)
    {
        // Limitamos el PWM entre -4095 y 4095
       float clamped = constrain(Sensor[i].PWM, -4095.0f, 4095.0f);
        // El motor debe moverse si:
        // 1. La sección está activa (FlowEnabled)
        // 2. O SI ESTAMOS EN MODO MANUAL (!AutoOn) -> Para el Test
        bool shouldRun = Sensor[i].FlowEnabled || !Sensor[i].AutoOn;
        // Lógica según tipo de actuador
        switch (Sensor[i].ControlType)
        {
        case StandardValve_ct:
        case Motor_ct:
        case Fan_ct:
           SetPWM(i, shouldRun ? clamped : 0.0f);
            break;

        case ComboClose_ct:
        case TimedCombo_ct:
            // Si es válvula combo, -4095 suele ser cerrar rápido
           
            SetPWM(i, shouldRun ? clamped : -4095.0f);
            break;

        default:
            SetPWM(i, 0.0f);
            break;
        }
    }
}

/**
 * Controla los drivers conectados a pines GPIO del ESP32.
 * Usa los canales LEDC configurados en Begin.cpp.
 */
void SetPWM(byte ID, float pwmVal) 
{
    // Validación básica
    if (ID >= MDL.SensorCount) return; 

    // 1. Obtener valor absoluto y limitar a 8 bits (0-4095)
    // En Begin.cpp configuramos ledcSetup(ch, 1000, 12); -> Resolución de 8 bits
    int duty = (int)fabs(pwmVal);
    if (duty > 4095) duty = 4095;

    // 2. Determinar los Canales LEDC correspondientes
    // Deben coincidir con la lógica de Begin.cpp:
    // Motor 0 -> Canales 0 y 1
    // Motor 1 -> Canales 2 y 3
    uint8_t ch1 = ID * 2;     
    uint8_t ch2 = ID * 2 + 1; 

    // 3. Escribir en los pines según la dirección
    if (pwmVal > 0) {
        // Giro Adelante
        ledcWrite(ch1, duty);
        ledcWrite(ch2, 0);
    } 
    else if (pwmVal < 0) {
        // Giro Atrás
        ledcWrite(ch1, 0);
        ledcWrite(ch2, duty);
    } 
    else {
        // Parada / Frenado
        ledcWrite(ch1, 0);
        ledcWrite(ch2, 0);
    }
    
    // --- DEBUG (Opcional, descomentar si hay problemas) ---
    /*
    static int lastD[2] = {-1,-1};
    if(abs(lastD[ID] - duty) > 5) {
        Serial.printf("⚡ Motor %d -> CH%d: %d | CH%d: %d\n", ID, ch1, (pwmVal>0?duty:0), ch2, (pwmVal<0?duty:0));
        lastD[ID] = duty;
    }
    */
}
void CheckCalibration() {
    for (int i = 0; i < MDL.SensorCount; i++) {
        
        if (Sensor[i].CalibActive) {
            
            // Como reseteamos a 0 al iniciar, TotalPulses ES el recorrido actual.
            if (Sensor[i].TotalPulses >= Sensor[i].CalibTargetPulses) {
                
                // ¡LLEGAMOS!
                Sensor[i].CalibActive = false;
                
                // Frenar
                Sensor[i].ManualAdjust = 0;
                Sensor[i].PWM = 0;
                SetPWM(i, 0); 
                
                // Volver a modo automático
                Sensor[i].AutoOn = true; 
                
                Serial.printf("✅ FIN CAL M%d: Llegó a %u pulsos.\n", i, Sensor[i].TotalPulses);
            }
        }
    }
}