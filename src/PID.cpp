#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"

// Estado PID por motor.
uint32_t LastCheck[MaxProductCount];
float LastPWM[MaxProductCount] = {0};
float IntegralSum[MaxProductCount];
float LastActual[MaxProductCount] = {0};
float TargetRamp[MaxProductCount] = {0};

void ResetPIDState(byte ID)
{
    IntegralSum[ID] = 0.0f;
    LastPWM[ID] = 0.0f;
    LastActual[ID] = Sensor[ID].UPM; // Evita spike del D al reiniciar
    TargetRamp[ID] = Sensor[ID].TargetUPM;
    Sensor[ID].PWM = 0;
    LastCheck[ID] = millis();
    Serial.printf("[PID] Reset Motor %d\n", ID);
}

void PIDmotor(byte ID)
{
    if (Sensor[ID].PIDtime < 10)
        Sensor[ID].PIDtime = 50;

    // --- MODO APAGADO O MANUAL ---
    // Target < 0.5 Hz se considera apagado (evita girar a MinPWM por FF con target~0)
    if (!Sensor[ID].FlowEnabled || Sensor[ID].TargetUPM < 0.5f)
    {
        IntegralSum[ID] = 0;
        LastPWM[ID] = 0;
        TargetRamp[ID] = 0;
        Sensor[ID].PWM = (!Sensor[ID].AutoOn) ? Sensor[ID].ManualAdjust : 0;
        SetPWM(ID, Sensor[ID].PWM);
        return;
    }

    // --- INTERVALO ADAPTATIVO ---
    bool isHyd = (Sensor[ID].MotorType == MOTOR_HYDRAULIC);
    uint32_t pidInterval = Sensor[ID].PIDtime;
    if (isHyd && pidInterval < 200) pidInterval = 200;

    if (millis() - LastCheck[ID] < pidInterval) return;

    float dt = (millis() - LastCheck[ID]) / 1000.0f;
    if (dt <= 0.0f) dt = 0.05f;
    LastCheck[ID] = millis();

    // --- TARGET RAMP (suaviza cambios bruscos de consigna) ---
    float targetSlewHz = Sensor[ID].TargetSlewHzPerSec;
    if (targetSlewHz <= 0) targetSlewHz = 50.0f;
    float targetDiff = Sensor[ID].TargetUPM - TargetRamp[ID];
    targetDiff = constrain(targetDiff, -targetSlewHz * dt, targetSlewHz * dt);
    TargetRamp[ID] += targetDiff;

    float target = TargetRamp[ID];
    float actual = Sensor[ID].UPM;
    float error = target - actual;

    // --- DEADBAND (más amplio para hidráulico) ---
    float deadband = Sensor[ID].Deadband;
    if (isHyd)
    {
        float minDB = Sensor[ID].HydHysteresis / 2.0f;
        if (minDB < 3.0f) minDB = 3.0f;
        if (deadband < minDB) deadband = minDB;
    }
    float errorPct = (target > 0) ? (fabs(error) / target) * 100.0f : 0;
    if (errorPct <= deadband) error = 0;

    // --- FEEDFORWARD (rango útil basePWM → MaxPWM) ---
    float maxHz = Sensor[ID].MaxHz;
    if (maxHz <= 0) maxHz = 40.0f;
    float ffGain = Sensor[ID].FFGain > 0 ? Sensor[ID].FFGain : 1.0f;
    float basePWM = (float)Sensor[ID].MinPWM;
    if (isHyd && Sensor[ID].HydDeadZonePWM > basePWM)
        basePWM = Sensor[ID].HydDeadZonePWM;
    float usableRange = (float)Sensor[ID].MaxPWM - basePWM;
    float ff = basePWM + (target / maxHz) * usableRange * ffGain;
    ff = constrain(ff, basePWM, (float)Sensor[ID].MaxPWM);

    // --- PROPORCIONAL ---
    float pTerm = error * Sensor[ID].Kp;

    // --- INTEGRAL con anti-windup ---
    bool saturadoAlto = (LastPWM[ID] >= Sensor[ID].MaxPWM && error > 0);
    bool saturadoBajo = (LastPWM[ID] <= Sensor[ID].MinPWM && error < 0);
    if (!saturadoAlto && !saturadoBajo && error != 0)
    {
        IntegralSum[ID] += (error * Sensor[ID].Ki * dt);
        float iLimit = Sensor[ID].MaxIntegral > 0 ? Sensor[ID].MaxIntegral : 1200.0f;
        IntegralSum[ID] = constrain(IntegralSum[ID], -iLimit, iLimit);
    }

    // --- DERIVATIVO sobre MEDICIÓN (evita derivative kick) ---
    float dTerm = 0;
    if (dt > 0 && Sensor[ID].Kd > 0)
    {
        dTerm = -Sensor[ID].Kd * (actual - LastActual[ID]) / dt;
    }
    LastActual[ID] = actual;

    // --- OUTPUT ---
    float output = ff + pTerm + IntegralSum[ID] + dTerm;

    // --- SLEW RATE (PWM/segundo, independiente de PIDtime) ---
    if (LastPWM[ID] == 0)
    {
        // Primer ciclo: saltar directo al feedforward
        output = ff;
    }
    else if (Sensor[ID].SlewRatePerSec > 0)
    {
        float maxChange = Sensor[ID].SlewRatePerSec * dt;
        output = constrain(output, LastPWM[ID] - maxChange, LastPWM[ID] + maxChange);
    }

    // Límites
    if (output > 0 && output < Sensor[ID].MinPWM)
        output = Sensor[ID].MinPWM;
    output = constrain(output, 0, (float)Sensor[ID].MaxPWM);

    // Aplicar
    LastPWM[ID] = output;
    Sensor[ID].PWM = (int)output;
    SetPWM(ID, Sensor[ID].PWM);

    // Debug serial
    extern bool mqttDebugEnabled;
    if (mqttDebugEnabled && ID == 0)
    {
        Serial.printf("PID[M0%s] TGT:%.1f(->%.1f) ACT:%.1f ERR:%.1f | FF:%.0f P:%.1f I:%.1f D:%.1f | OUT:%.0f\n",
                      isHyd ? " HYD" : " ELC",
                      Sensor[ID].TargetUPM, target, actual, error,
                      ff, pTerm, IntegralSum[ID], dTerm, output);
    }
}
