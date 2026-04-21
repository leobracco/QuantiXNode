#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"
#include "Log.h"
#include "core/PidCore.h"

// Variables de estado del PID (una por motor).
uint32_t LastCheck[MaxProductCount];
quantix::core::PidState g_pidState[MaxProductCount] = {};

// Aliases expuestos para no romper otros módulos que pudieran leerlos.
float LastPWM[MaxProductCount] = {0};
float IntegralSum[MaxProductCount] = {0};

void ResetPIDState(byte ID)
{
    g_pidState[ID].integralSum = 0.0f;
    g_pidState[ID].lastOut = 0.0f;
    IntegralSum[ID] = 0.0f;
    LastPWM[ID] = 0.0f;
    Sensor[ID].PWM = 0;
    LastCheck[ID] = millis();
    LOG_I("pid", "Reset estado motor %u", (unsigned)ID);
}

void PIDmotor(byte ID)
{
    if (Sensor[ID].PIDtime < 10)
        Sensor[ID].PIDtime = 50;

    // --- 1. MODO APAGADO O MANUAL ---
    if (!Sensor[ID].FlowEnabled || Sensor[ID].TargetUPM <= 0)
    {
        g_pidState[ID].integralSum = 0.0f;
        g_pidState[ID].lastOut = 0.0f;
        IntegralSum[ID] = 0;
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
        if (dt <= 0.0f) dt = 0.05f;
        LastCheck[ID] = millis();

        quantix::core::PidGains gains;
        gains.kp          = Sensor[ID].Kp;
        gains.ki          = Sensor[ID].Ki;
        gains.kd          = Sensor[ID].Kd;
        gains.minOut      = (float)Sensor[ID].MinPWM;
        gains.maxOut      = (float)Sensor[ID].MaxPWM;
        gains.maxIntegral = Sensor[ID].MaxIntegral > 0 ? Sensor[ID].MaxIntegral : gains.maxOut;
        gains.deadbandPct = (float)Sensor[ID].Deadband;
        gains.slewRate    = (float)Sensor[ID].SlewRate;

        float output = quantix::core::computePidStep(
            Sensor[ID].TargetUPM, Sensor[ID].UPM, dt, gains, g_pidState[ID]);

        // Sincronizar aliases legacy que otros módulos pudieran observar.
        IntegralSum[ID] = g_pidState[ID].integralSum;
        LastPWM[ID]     = g_pidState[ID].lastOut;

        Sensor[ID].PWM = (int)output;
        SetPWM(ID, Sensor[ID].PWM);

        LOG_D("pid", "M%u TGT:%.1f ACT:%.1f I:%.1f OUT:%.0f",
              (unsigned)ID, Sensor[ID].TargetUPM, Sensor[ID].UPM,
              g_pidState[ID].integralSum, output);
    }
}