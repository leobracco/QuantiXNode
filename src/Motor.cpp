#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"
#include "Constants.h"
#include "Log.h"
#include "core/ControlType.h"

using quantix::core::ControlType;

// Función para ajustar el flujo general (Llamada desde main)
void AdjustFlow()
{
    const float kPwmMaxF = (float)quantix::constants::PWM_MAX;
    for (int i = 0; i < MDL.SensorCount; i++)
    {
        float clamped = constrain(Sensor[i].PWM, -kPwmMaxF, kPwmMaxF);
        // El motor debe moverse si:
        // 1. La sección está activa (FlowEnabled)
        // 2. O SI ESTAMOS EN MODO MANUAL (!AutoOn) -> Para el Test
        bool shouldRun = Sensor[i].FlowEnabled || !Sensor[i].AutoOn;
        // Lógica según tipo de actuador
        switch (static_cast<ControlType>(Sensor[i].ControlType))
        {
        case ControlType::StandardValve:
        case ControlType::Motor:
        case ControlType::Fan:
            SetPWM(i, shouldRun ? clamped : 0.0f);
            break;

        case ControlType::ComboClose:
        case ControlType::TimedCombo:
            // Válvula combo: PWM máximo negativo cierra rápido
            SetPWM(i, shouldRun ? clamped : -kPwmMaxF);
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
    if (ID >= MDL.SensorCount)
        return;

    // PCA9685 / LEDC resolución 12 bits: 0..PWM_MAX
    int duty = (int)fabs(pwmVal);
    if (duty > (int)quantix::constants::PWM_MAX)
        duty = (int)quantix::constants::PWM_MAX;

    uint8_t ch1 = ID * 2;
    uint8_t ch2 = ID * 2 + 1;

    if (pwmVal > 0)
    {
        ledcWrite(ch1, duty);
        ledcWrite(ch2, 0);
    }
    else if (pwmVal < 0)
    {
        ledcWrite(ch1, 0);
        ledcWrite(ch2, duty);
    }
    else
    {
        ledcWrite(ch1, 0);
        ledcWrite(ch2, 0);
    }
}
void CheckCalibration()
{
    for (int i = 0; i < MDL.SensorCount; i++)
    {
        if (!Sensor[i].CalibrandoAhora)
            continue;

        noInterrupts();
        uint32_t totalPulsesSnapshot = Sensor[i].TotalPulses;
        interrupts();

        // Como reseteamos a 0 al iniciar, TotalPulses ES el recorrido actual.
        if (totalPulsesSnapshot >= Sensor[i].PulsosMetaCalibracion)
        {
            Sensor[i].CalibrandoAhora = false;
            Sensor[i].ManualAdjust = 0;
            Sensor[i].PWM = 0;
            SetPWM(i, 0);
            Sensor[i].AutoOn = true;

            LOG_I("cal", "Fin calibración M%d, pulsos=%lu",
                  i, (unsigned long)totalPulsesSnapshot);
        }
    }
}