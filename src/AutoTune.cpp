// ============================================================================
// AutoTune.cpp - Calibración automática de PID por relay feedback
// Método Åström-Hägglund: oscila el PWM entre min y max, mide período
// y amplitud de la respuesta, calcula Kp/Ki/Kd con Ziegler-Nichols.
// ============================================================================

#include <Arduino.h>
#include <PubSubClient.h>
#include "Globals.h"
#include "Structs.h"

// Estado del auto-tune por motor.
struct AutoTuneState
{
    bool active;
    uint32_t startTime;
    float relayHigh;     // PWM alto (MaxPWM * 0.7)
    float relayLow;      // PWM bajo (MinPWM)
    float setpoint;      // Hz objetivo (punto medio de operación)

    // Detección de cruces por cero.
    bool aboveSetpoint;
    uint32_t lastCrossing;
    int crossingCount;

    // Mediciones de oscilación.
    float peakHigh;
    float peakLow;
    float periodSum;
    int periodCount;

    // Resultados.
    float Ku;   // Ganancia última
    float Tu;   // Período último (ms)
    float calcKp, calcKi, calcKd;
    bool done;
    bool failed;
};

static AutoTuneState _at[MaxProductCount];

// Publica resultados vía MQTT.
extern char uid[13];
extern PubSubClient mqttClient;

static void PublishResult(byte ID)
{
    if (!mqttClient.connected()) return;

    char topic[64];
    sprintf(topic, "agp/quantix/%s/autotune_result", uid);

    char buf[256];
    if (_at[ID].failed)
    {
        sprintf(buf, "{\"id\":%d,\"ok\":false,\"msg\":\"No se detectaron oscilaciones suficientes\"}", ID);
    }
    else
    {
        sprintf(buf, "{\"id\":%d,\"ok\":true,\"kp\":%.1f,\"ki\":%.1f,\"kd\":%.1f,\"ku\":%.1f,\"tu\":%.0f}",
                ID, _at[ID].calcKp, _at[ID].calcKi, _at[ID].calcKd,
                _at[ID].Ku, _at[ID].Tu);
    }
    mqttClient.publish(topic, buf);
    Serial.printf("[AutoTune] M%d resultado: %s\n", ID, buf);
}

// Iniciar auto-tune para un motor.
void AutoTuneStart(byte ID)
{
    if (ID >= MaxProductCount) return;
    auto &at = _at[ID];

    memset(&at, 0, sizeof(AutoTuneState));
    at.active = true;
    at.startTime = millis();

    // Relay entre MinPWM y 70% de MaxPWM.
    at.relayLow = (float)Sensor[ID].MinPWM;
    at.relayHigh = (float)Sensor[ID].MaxPWM * 0.7f;

    // Setpoint: Hz correspondiente al punto medio del rango.
    float maxHz = Sensor[ID].MaxHz > 0 ? Sensor[ID].MaxHz : 40.0f;
    at.setpoint = maxHz * 0.4f; // 40% de la capacidad.

    at.aboveSetpoint = false;
    at.peakHigh = 0;
    at.peakLow = 9999;

    // Desactivar PID normal.
    Sensor[ID].AutoOn = false;
    Sensor[ID].FlowEnabled = false;

    // Arrancar con PWM alto.
    SetPWM(ID, (int)at.relayHigh);

    Serial.printf("[AutoTune] M%d INICIO: setpoint=%.1f Hz, relay=%.0f/%.0f PWM\n",
                  ID, at.setpoint, at.relayLow, at.relayHigh);
}

// Detener auto-tune.
void AutoTuneStop(byte ID)
{
    if (ID >= MaxProductCount) return;
    _at[ID].active = false;
    SetPWM(ID, 0);
    Sensor[ID].AutoOn = true;
    Serial.printf("[AutoTune] M%d DETENIDO\n", ID);
}

static void CalculateAndPublish(byte ID)
{
    auto &at = _at[ID];

    at.Tu = at.periodSum / (float)at.periodCount;

    float amplitude = (at.peakHigh - at.peakLow) / 2.0f;
    if (amplitude < 0.1f) amplitude = 0.1f;

    float d = (at.relayHigh - at.relayLow) / 2.0f;

    at.Ku = (4.0f * d) / (3.14159f * amplitude);

    float TuSec = at.Tu / 1000.0f;
    at.calcKp = 0.6f * at.Ku;
    at.calcKi = 2.0f * at.calcKp / TuSec;
    at.calcKd = at.calcKp * TuSec / 8.0f;

    at.calcKp = constrain(at.calcKp, 0.1f, 500.0f);
    at.calcKi = constrain(at.calcKi, 0.1f, 200.0f);
    at.calcKd = constrain(at.calcKd, 0.0f, 100.0f);

    Serial.printf("[AutoTune] M%d RESULTADO: Ku=%.1f Tu=%.0fms -> Kp=%.1f Ki=%.1f Kd=%.1f\n",
                  ID, at.Ku, at.Tu, at.calcKp, at.calcKi, at.calcKd);

    PublishResult(ID);
}

// Llamar desde el loop principal (cada 50ms, después de GetUPM).
void AutoTuneTick(byte ID)
{
    if (ID >= MaxProductCount || !_at[ID].active) return;
    auto &at = _at[ID];

    float actual = Sensor[ID].Hz;
    uint32_t now = millis();

    // Timeout: 30 segundos máximo.
    if (now - at.startTime > 30000)
    {
        at.active = false;
        SetPWM(ID, 0);
        Sensor[ID].AutoOn = true;

        if (at.periodCount >= 3)
        {
            at.done = true;
            CalculateAndPublish(ID);
        }
        else
        {
            at.failed = true;
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

        // Relay: cambiar PWM.
        if (nowAbove)
            SetPWM(ID, (int)at.relayLow);
        else
            SetPWM(ID, (int)at.relayHigh);

        // Medir período (cada 2 cruces = 1 ciclo completo).
        at.crossingCount++;
        if (at.crossingCount >= 3 && at.lastCrossing > 0)
        {
            if (at.crossingCount % 2 == 1)
            {
                float period = (float)(now - at.lastCrossing);
                if (period > 200 && period < 15000)
                {
                    at.periodSum += period;
                    at.periodCount++;
                    Serial.printf("[AutoTune] M%d ciclo %d: T=%.0f ms, peak=%.1f/%.1f Hz\n",
                                  ID, at.periodCount, period, at.peakLow, at.peakHigh);
                }
            }
            at.lastCrossing = now;
        }
        else
        {
            at.lastCrossing = now;
        }

        // Si tenemos suficientes ciclos, terminar.
        if (at.periodCount >= 5)
        {
            at.done = true;
            at.active = false;
            SetPWM(ID, 0);
            Sensor[ID].AutoOn = true;
            CalculateAndPublish(ID);
        }
    }
}

bool AutoTuneActive(byte ID)
{
    return ID < MaxProductCount && _at[ID].active;
}
