#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"

// Variables de estado del PID
uint32_t LastCheck[MaxProductCount];
float LastPWM[MaxProductCount] = {0};
float IntegralSum[MaxProductCount];

void ResetPIDState(byte ID)
{
    IntegralSum[ID] = 0.0f;
    LastPWM[ID] = 0.0f;
    Sensor[ID].PWM = 0;
    LastCheck[ID] = millis();
    Serial.printf("[PID] Reset Estado Motor %d\n", ID);
}

void PIDmotor(byte ID)
{
    if (Sensor[ID].PIDtime < 10)
        Sensor[ID].PIDtime = 50;

    // --- 1. MODO APAGADO O MANUAL ---
    if (!Sensor[ID].FlowEnabled || Sensor[ID].TargetUPM <= 0)
    {
        IntegralSum[ID] = 0; // Limpiamos memoria
        LastPWM[ID] = 0;

        // Si no está en automático, usa el slider manual de la web. Si no, 0.
        Sensor[ID].PWM = (!Sensor[ID].AutoOn) ? Sensor[ID].ManualAdjust : 0;
        SetPWM(ID, Sensor[ID].PWM);
        return;
    }

    // --- 2. CÁLCULO PID AUTOMÁTICO ---
    if (millis() - LastCheck[ID] >= Sensor[ID].PIDtime)
    {
        float dt = (millis() - LastCheck[ID]) / 1000.0f;
        if (dt <= 0.0)
            dt = 0.05f;
        LastCheck[ID] = millis();

        float target = Sensor[ID].TargetUPM;
        float actual = Sensor[ID].UPM;
        float error = target - actual;

        // A) Zona Muerta (Evita que el motor "tiemble" cuando ya llegó)
        float errorPct = (fabs(error) / target) * 100.0f;
        if (errorPct <= Sensor[ID].Deadband)
        {
            error = 0;
        }

        // B) Término Proporcional (Reacción inmediata)
        float pTerm = error * Sensor[ID].Kp;

        // C) Término Integral (Fuerza acumulativa) con ANTI-WINDUP
        // Solo integramos si no estamos chocando contra los límites de hardware
        bool saturadoAlto = (LastPWM[ID] >= Sensor[ID].MaxPWM && error > 0);
        bool saturadoBajo = (LastPWM[ID] <= Sensor[ID].MinPWM && error < 0);

        if (!saturadoAlto && !saturadoBajo && error != 0)
        {
            IntegralSum[ID] += (error * Sensor[ID].Ki * dt);

            // Límite de seguridad para la memoria integral
            float iLimit = Sensor[ID].MaxIntegral > 0 ? Sensor[ID].MaxIntegral : 4095.0f;
            IntegralSum[ID] = constrain(IntegralSum[ID], -iLimit, iLimit);
        }

        // D) Salida Teórica
        float output = pTerm + IntegralSum[ID];

        // E) Vencer Inercia: Si calculó algo mayor a 0, pero menor al mínimo de arranque, forzamos el mínimo
        if (output > 0 && output < Sensor[ID].MinPWM)
        {
            output = Sensor[ID].MinPWM;
        }

        // F) Slew Rate (Aceleración controlada para cuidar los engranajes)
        if (Sensor[ID].SlewRate > 0)
        {
            float maxChange = Sensor[ID].SlewRate;
            if (output > LastPWM[ID] + maxChange)
                output = LastPWM[ID] + maxChange;
            if (output < LastPWM[ID] - maxChange)
                output = LastPWM[ID] - maxChange;
        }

        // G) Límite Físico Final
        output = constrain(output, Sensor[ID].MinPWM, Sensor[ID].MaxPWM);

        // --- 3. APLICAR AL HARDWARE ---
        LastPWM[ID] = output;
        Sensor[ID].PWM = (int)output;
        SetPWM(ID, Sensor[ID].PWM);

        // --- 4. DEBUG DE ALTA CALIDAD ---
        if (ID == 0)
        {
            Serial.printf("⚙️ PID[M0] TGT:%.1f ACT:%.1f | ERR:%.1f | P:%.1f I:%.1f | OUT:%.0f\n",
                          target, actual, error, pTerm, IntegralSum[ID], output);
        }
    }
}