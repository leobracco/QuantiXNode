// ============================================================================
// AutoTune.cpp - Calibración automática de PID por relay feedback
// ============================================================================

#include <Arduino.h>
#include <PubSubClient.h>
#include "Globals.h"
#include "Structs.h"

struct AutoTuneState
{
    bool active;
    uint32_t startTime;
    float relayHigh;
    float relayLow;
    float setpoint;

    bool aboveSetpoint;
    bool relayIsHigh;
    uint32_t lastCrossUp;   // Último cruce de abajo→arriba
    int fullCycles;         // Ciclos completos (arriba+abajo)

    float peakHigh;
    float peakLow;
    float periodSum;

    float Ku, Tu;
    float calcKp, calcKi, calcKd;
    bool done;
    bool failed;
};

static AutoTuneState _at[MaxProductCount];

extern char uid[13];
extern PubSubClient mqttClient;

static void PublishResult(byte ID)
{
    if (!mqttClient.connected()) return;
    char topic[64];
    sprintf(topic, "agp/quantix/%s/autotune_result", uid);

    char buf[256];
    if (_at[ID].failed)
        sprintf(buf, "{\"id\":%d,\"ok\":false,\"msg\":\"No se detectaron oscilaciones suficientes\"}", ID);
    else
        sprintf(buf, "{\"id\":%d,\"ok\":true,\"kp\":%.1f,\"ki\":%.1f,\"kd\":%.1f,\"ku\":%.1f,\"tu\":%.0f}",
                ID, _at[ID].calcKp, _at[ID].calcKi, _at[ID].calcKd, _at[ID].Ku, _at[ID].Tu);

    mqttClient.publish(topic, buf);
    Serial.printf("[AutoTune] M%d resultado: %s\n", ID, buf);
}

static void CalculateAndPublish(byte ID)
{
    auto &at = _at[ID];
    at.Tu = at.periodSum / (float)at.fullCycles;

    float amplitude = (at.peakHigh - at.peakLow) / 2.0f;
    if (amplitude < 0.1f) amplitude = 0.1f;

    float d = (at.relayHigh - at.relayLow) / 2.0f;
    at.Ku = (4.0f * d) / (3.14159f * amplitude);

    float TuSec = at.Tu / 1000.0f;
    at.calcKp = 0.6f * at.Ku;
    at.calcKi = (TuSec > 0.01f) ? 2.0f * at.calcKp / TuSec : at.calcKp;
    at.calcKd = at.calcKp * TuSec / 8.0f;

    at.calcKp = constrain(at.calcKp, 0.1f, 500.0f);
    at.calcKi = constrain(at.calcKi, 0.1f, 200.0f);
    at.calcKd = constrain(at.calcKd, 0.0f, 100.0f);

    Serial.printf("[AutoTune] M%d LISTO: Ku=%.1f Tu=%.0fms -> Kp=%.1f Ki=%.1f Kd=%.1f\n",
                  ID, at.Ku, at.Tu, at.calcKp, at.calcKi, at.calcKd);
    PublishResult(ID);
}

void AutoTuneStart(byte ID)
{
    if (ID >= MaxProductCount) return;
    auto &at = _at[ID];
    memset(&at, 0, sizeof(AutoTuneState));
    at.active = true;
    at.startTime = millis();

    at.relayLow = (float)Sensor[ID].MinPWM;
    at.relayHigh = (float)Sensor[ID].MaxPWM * 0.7f;
    if (at.relayHigh < at.relayLow + 200) at.relayHigh = at.relayLow + 500;

    // Setpoint: leer Hz actual si el motor ya gira, sino usar MaxHz * 0.3
    float currentHz = Sensor[ID].Hz;
    if (currentHz > 1.0f)
        at.setpoint = currentHz * 0.8f; // Un poco debajo del actual
    else
    {
        float maxHz = Sensor[ID].MaxHz > 0 ? Sensor[ID].MaxHz : 40.0f;
        at.setpoint = maxHz * 0.3f;
    }

    at.peakHigh = 0;
    at.peakLow = 9999;
    at.relayIsHigh = true;

    Sensor[ID].AutoOn = false;
    Sensor[ID].FlowEnabled = false;

    // Arrancar con PWM alto.
    Sensor[ID].PWM = (int)at.relayHigh;
    SetPWM(ID, (int)at.relayHigh);

    Serial.printf("[AutoTune] M%d INICIO: setpoint=%.1f Hz, relay=%.0f/%.0f PWM, Hz actual=%.1f\n",
                  ID, at.setpoint, at.relayLow, at.relayHigh, currentHz);
}

void AutoTuneStop(byte ID)
{
    if (ID >= MaxProductCount) return;
    _at[ID].active = false;
    Sensor[ID].PWM = 0;
    SetPWM(ID, 0);
    Sensor[ID].AutoOn = true;
    Serial.printf("[AutoTune] M%d DETENIDO\n", ID);
}

void AutoTuneTick(byte ID)
{
    if (ID >= MaxProductCount || !_at[ID].active) return;
    auto &at = _at[ID];

    float actual = Sensor[ID].Hz;
    uint32_t now = millis();

    // Timeout: 45 segundos máximo.
    if (now - at.startTime > 45000)
    {
        at.active = false;
        Sensor[ID].PWM = 0;
        SetPWM(ID, 0);
        Sensor[ID].AutoOn = true;

        if (at.fullCycles >= 2)
        {
            at.done = true;
            CalculateAndPublish(ID);
        }
        else
        {
            at.failed = true;
            Serial.printf("[AutoTune] M%d TIMEOUT: solo %d ciclos (necesita 2+)\n", ID, at.fullCycles);
            PublishResult(ID);
        }
        return;
    }

    // Trackear picos.
    if (actual > at.peakHigh) at.peakHigh = actual;
    if (actual < at.peakLow && actual > 0.1f) at.peakLow = actual;

    // Detección de cruce por setpoint.
    bool nowAbove = actual > at.setpoint;
    if (nowAbove != at.aboveSetpoint)
    {
        at.aboveSetpoint = nowAbove;

        // Conmutar relay.
        if (nowAbove)
        {
            // Cruzó hacia arriba → poner PWM bajo.
            Sensor[ID].PWM = (int)at.relayLow;
            SetPWM(ID, (int)at.relayLow);
            at.relayIsHigh = false;

            // Medir período: tiempo entre dos cruces ascendentes.
            if (at.lastCrossUp > 0)
            {
                float period = (float)(now - at.lastCrossUp);
                if (period > 300 && period < 20000)
                {
                    at.periodSum += period;
                    at.fullCycles++;
                    Serial.printf("[AutoTune] M%d ciclo %d: T=%.0fms peak=%.1f/%.1f Hz\n",
                                  ID, at.fullCycles, period, at.peakLow, at.peakHigh);
                }
            }
            at.lastCrossUp = now;
        }
        else
        {
            // Cruzó hacia abajo → poner PWM alto.
            Sensor[ID].PWM = (int)at.relayHigh;
            SetPWM(ID, (int)at.relayHigh);
            at.relayIsHigh = true;
        }

        Serial.printf("[AutoTune] M%d cruce: Hz=%.1f %s setpoint=%.1f -> PWM=%s\n",
                      ID, actual, nowAbove ? "ARRIBA" : "ABAJO",
                      at.setpoint, at.relayIsHigh ? "HIGH" : "LOW");
    }

    // Si tenemos suficientes ciclos completos, terminar.
    if (at.fullCycles >= 4)
    {
        at.done = true;
        at.active = false;
        Sensor[ID].PWM = 0;
        SetPWM(ID, 0);
        Sensor[ID].AutoOn = true;
        CalculateAndPublish(ID);
    }
}

bool AutoTuneActive(byte ID)
{
    return ID < MaxProductCount && _at[ID].active;
}
