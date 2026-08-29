// ============================================================================
// CalibAuto.cpp — Calibración manual del caudalímetro + Auto-Tune PID.
//
// Comandos disparados desde AOG (FlowXController.cs) vía MQTT en
//   agp/flow/<uid>/cmd/<verb>
// y resueltos por mqttCallback() en MQTT_Custom.cpp, que llama a estas APIs:
//
//   CalibStart(ID, vol_l, pwm)     — abre la bomba a PWM fijo, cuenta pulsos
//   CalibStop(ID, cancelled)       — cierra y publica calibrar_result
//   AutoTuneStart(ID, sp, hi, lo)  — arranca Ziegler-Nichols por relay feedback
//   AutoTuneStop(ID, cancelled)    — corta y publica autotune_result
//   RunCalibAuto(ID)               — llamado cada tick desde main.loop()
//
// Estado por canal en variables estáticas (no contamina Structs.h).
//
// El bridge AOG (FlowXBridge) actualmente solo gestiona Productos[0] por nodo,
// pero acá soportamos hasta MaxProductCount por si en el futuro se amplía.
// ============================================================================

#include <Arduino.h>
#include <math.h>
#include "Globals.h"

// --- Estado interno por canal ---
struct CalibState
{
    bool active;
    uint32_t startMs;
    int pwm;        // PWM aplicado durante la calibración
    float volL;     // volumen objetivo (informativo, AOG calcula meter_cal)
};
static CalibState CAL[MaxProductCount];

enum AtPhase : uint8_t
{
    AT_IDLE = 0,
    AT_WAIT_HIGH, // PWM alto, esperando que UPM supere setpoint
    AT_WAIT_LOW   // PWM bajo, esperando que UPM baje del setpoint
};

struct AutoTuneState
{
    bool active;
    AtPhase phase;
    float setpointHz; // setpoint en Hz (UPM)
    int pwmHigh;
    int pwmLow;

    uint32_t startMs;
    uint32_t lastCrossMs;
    uint32_t tuSumMs; // suma de períodos para promediar Tu
    uint8_t tuSamples;

    float minUpm; // UPM mínima observada (para amplitud "a")
    float maxUpm; // UPM máxima observada
};
static AutoTuneState AT[MaxProductCount];

// Parámetros del auto-tune (constantes razonables para una bomba típica).
static const uint32_t AT_TIMEOUT_MS = 30000; // 30 s máximo
static const uint8_t  AT_MIN_SAMPLES = 4;    // 4 períodos completos
static const uint8_t  AT_MAX_SAMPLES = 8;

// --- Caracterización motor (PWM_min + Hz_max) ---
// Rampa PWM 0..4095 en N pasos uniformes con dwell por paso. En cada paso
// muestreamos Hz (UPM) tras dejar asentar la bomba y guardamos el par
// (pwm, hz). Al terminar, derivamos:
//   pwm_min        = primer PWM donde Hz > 0.5 (flujo detectado)
//   pwm_min_estable= primer PWM donde Hz > 5.0 (mínimo "útil" de PID)
//   hz_max         = máximo Hz observado (típicamente en el último paso)
// La curva se publica truncada a CHAR_CURVE_LEN puntos para que el payload
// MQTT entre cómodo (~600 bytes).
static const uint8_t  CHAR_STEPS = 30;        // 30 pasos uniformes
static const uint16_t CHAR_DWELL_MS = 500;    // 500ms por paso → ~15s total
static const uint32_t CHAR_TIMEOUT_MS = 60000; // hard timeout: 60s
static const uint8_t  CHAR_CURVE_LEN = 30;    // máximo de puntos en el payload

struct CharState
{
    bool active;
    uint32_t startMs;
    uint8_t step;           // paso actual (0..CHAR_STEPS-1)
    uint32_t stepStartMs;   // ts del paso actual
    int pwmCurve[CHAR_CURVE_LEN];
    float hzCurve[CHAR_CURVE_LEN];
    int curveLen;
};
static CharState CH[MaxProductCount];

// ----------------------------------------------------------------------------

bool IsCalibActive(byte ID)
{
    if (ID >= MaxProductCount) return false;
    return CAL[ID].active;
}

bool IsAutoTuneActive(byte ID)
{
    if (ID >= MaxProductCount) return false;
    return AT[ID].active;
}

// --- Calibración manual --------------------------------------------------

void CalibStart(byte ID, float volL, int pwm)
{
    if (ID >= MaxProductCount) return;

    // Cancelamos cualquier autotune en curso para no pelear por el PWM.
    if (AT[ID].active) AutoTuneStop(ID, true);

    CAL[ID].active = true;
    CAL[ID].startMs = millis();
    CAL[ID].pwm = constrain(pwm, 0, 4095);
    CAL[ID].volL = (volL > 0.0f) ? volL : 1.0f;

    // Reset de pulsos del canal y marcamos la estructura de Sensor[] como
    // calibrando (main.cpp ya respeta CalibActive en su loop y aplica
    // ManualAdjust en lugar de PID).
    Sensor[ID].TotalPulses = 0;
    Sensor[ID].CalibActive = true;
    Sensor[ID].CalibTargetPulses = 0; // sin auto-stop por pulsos
    Sensor[ID].ManualAdjust = CAL[ID].pwm;
    Sensor[ID].PidState = 0;

    Serial.printf("[CAL] start ID=%u pwm=%d vol_l=%.3f\n", ID, CAL[ID].pwm, CAL[ID].volL);
}

void CalibStop(byte ID, bool cancelled)
{
    if (ID >= MaxProductCount) return;
    if (!CAL[ID].active && !Sensor[ID].CalibActive) return;

    uint32_t dur = millis() - CAL[ID].startMs;
    uint32_t pulsos = Sensor[ID].TotalPulses;

    CAL[ID].active = false;
    Sensor[ID].CalibActive = false;
    Sensor[ID].ManualAdjust = 0;
    SetPWM(ID, 0);

    const char *err = cancelled ? "cancelled" : "";
    publishCalibResult(ID, !cancelled, pulsos, dur, err);
    Serial.printf("[CAL] stop ID=%u pulsos=%u dur=%ums cancelled=%d\n",
                  ID, pulsos, dur, (int)cancelled);
}

// --- Auto-Tune (Ziegler-Nichols por relay feedback) ----------------------

void AutoTuneStart(byte ID, float setpointHz, int pwmHigh, int pwmLow)
{
    if (ID >= MaxProductCount) return;

    if (CAL[ID].active) CalibStop(ID, true);

    AT[ID].active = true;
    AT[ID].phase = AT_WAIT_HIGH;
    AT[ID].setpointHz = (setpointHz > 0.0f) ? setpointHz : Sensor[ID].TargetUPM;
    AT[ID].pwmHigh = constrain(pwmHigh > 0 ? pwmHigh : Sensor[ID].MaxPWM, 0, 4095);
    AT[ID].pwmLow  = constrain(pwmLow  >= 0 ? pwmLow  : Sensor[ID].MinPWM, 0, 4095);
    if (AT[ID].pwmHigh <= AT[ID].pwmLow)
        AT[ID].pwmHigh = AT[ID].pwmLow + 100;

    AT[ID].startMs = millis();
    AT[ID].lastCrossMs = 0;
    AT[ID].tuSumMs = 0;
    AT[ID].tuSamples = 0;
    AT[ID].minUpm = 1e9f;
    AT[ID].maxUpm = -1e9f;

    // Desactivamos el PID propio para este canal durante el tuning: el loop
    // ve AutoTuneActive y nos cede el PWM.
    Sensor[ID].FlowEnabled = true; // forzamos que el PWM no quede en 0 por el guard del PID
    SetPWM(ID, AT[ID].pwmHigh);
    Sensor[ID].PWM = AT[ID].pwmHigh; // telemetría: que el widget muestre el PWM real del tuning

    Serial.printf("[AT] start ID=%u sp=%.2fHz hi=%d lo=%d\n",
                  ID, AT[ID].setpointHz, AT[ID].pwmHigh, AT[ID].pwmLow);
}

void AutoTuneStop(byte ID, bool cancelled)
{
    if (ID >= MaxProductCount) return;
    if (!AT[ID].active) return;

    AT[ID].active = false;
    SetPWM(ID, 0);

    if (cancelled || AT[ID].tuSamples < AT_MIN_SAMPLES)
    {
        const char *err = cancelled ? "cancelled" : "insufficient_samples";
        publishAutoTuneResult(ID, false, 0, 0, 0, 0, 0, err);
        Serial.printf("[AT] stop ID=%u FAIL samples=%u\n", ID, AT[ID].tuSamples);
        return;
    }

    // Tu: período medio observado en ms → seg
    float tuMs = (float)AT[ID].tuSumMs / (float)AT[ID].tuSamples;
    float tuS = tuMs / 1000.0f;

    // Amplitud de salida (a) en UPM, amplitud de entrada (h) en counts PWM.
    float a = (AT[ID].maxUpm - AT[ID].minUpm) * 0.5f;
    float h = (float)(AT[ID].pwmHigh - AT[ID].pwmLow) * 0.5f;

    if (a <= 0.001f)
    {
        publishAutoTuneResult(ID, false, 0, 0, 0, 0, tuMs, "no_amplitude");
        return;
    }

    // Ku ≈ 4·h / (π·a)  (Åström-Hägglund describing-function)
    float ku = (4.0f * h) / (PI * a);

    // Ziegler-Nichols PID clásico:
    //   Kp = 0.6·Ku, Ki = 1.2·Ku/Tu, Kd = 0.075·Ku·Tu
    float kp = 0.6f * ku;
    float ki = (tuS > 0.001f) ? (1.2f * ku / tuS) : 0.0f;
    float kd = 0.075f * ku * tuS;

    // Aplicamos al canal para que el PID los use desde ya.
    Sensor[ID].Kp = kp;
    Sensor[ID].Ki = ki;
    Sensor[ID].Kd = kd;

    publishAutoTuneResult(ID, true, kp, ki, kd, ku, tuMs, "");
    Serial.printf("[AT] OK ID=%u Ku=%.3f Tu=%.0fms Kp=%.3f Ki=%.3f Kd=%.3f\n",
                  ID, ku, tuMs, kp, ki, kd);
}

void RunCalibAuto(byte ID)
{
    if (ID >= MaxProductCount) return;

    // Calibración manual: PWM lo aplica el main.loop() vía ManualAdjust.
    // Acá solo chequeamos timeout de seguridad (5 min).
    if (CAL[ID].active)
    {
        if (millis() - CAL[ID].startMs > 5UL * 60UL * 1000UL)
        {
            Serial.println("[CAL] timeout 5min — auto-stop");
            CalibStop(ID, true);
        }
        return;
    }

    if (!AT[ID].active) return;

    // --- Auto-tune ---
    // Timeout duro
    if (millis() - AT[ID].startMs > AT_TIMEOUT_MS)
    {
        AutoTuneStop(ID, false); // intenta cerrar con lo que haya
        return;
    }

    float upm = Sensor[ID].UPM;
    if (upm > AT[ID].maxUpm) AT[ID].maxUpm = upm;
    if (upm < AT[ID].minUpm) AT[ID].minUpm = upm;

    uint32_t now = millis();

    switch (AT[ID].phase)
    {
    case AT_WAIT_HIGH:
        // Estamos forzando PWM alto esperando cruce ascendente del setpoint.
        if (upm >= AT[ID].setpointHz)
        {
            if (AT[ID].lastCrossMs != 0)
            {
                AT[ID].tuSumMs += (now - AT[ID].lastCrossMs);
                AT[ID].tuSamples++;
            }
            AT[ID].lastCrossMs = now;
            AT[ID].phase = AT_WAIT_LOW;
            SetPWM(ID, AT[ID].pwmLow);
            Sensor[ID].PWM = AT[ID].pwmLow; // telemetría refleja el PWM real
        }
        break;

    case AT_WAIT_LOW:
        if (upm <= AT[ID].setpointHz)
        {
            // Mitad de período: no contamos para Tu (Tu = subida→subida)
            AT[ID].phase = AT_WAIT_HIGH;
            SetPWM(ID, AT[ID].pwmHigh);
            Sensor[ID].PWM = AT[ID].pwmHigh; // telemetría refleja el PWM real
        }
        break;

    default:
        break;
    }

    if (AT[ID].tuSamples >= AT_MAX_SAMPLES)
        AutoTuneStop(ID, false);
}

// --- Caracterización motor (pwm_min + hz_max) ----------------------------
// Pre-condición de campo: master abierto y al menos una sección — sin flujo
// real la rampa mide solo ruido del PWM y los puntos no son útiles. El
// chequeo se hace en CharStart() y se vuelve a chequear en RunChar() por si
// el operario cierra mid-rampa.

bool IsCharActive(byte ID)
{
    if (ID >= MaxProductCount) return false;
    return CH[ID].active;
}

void CharStart(byte ID)
{
    if (ID >= MaxProductCount) return;

    // Pre-check: necesitamos secciones abiertas. Sin flujo real medimos cero
    // en todos los pasos y derivaríamos pwm_min=0/hz_max=0, peor que nada.
    if (seccionesBits == 0)
    {
        publishCharResult(ID, false, 0, 0, 0.0f, 0.0f, nullptr, nullptr, 0,
                          "sections_closed");
        Serial.println("[CHAR] abort: secciones cerradas");
        return;
    }

    // Cancelamos cualquier calibración / autotune en curso — todos pelean
    // por el mismo PWM del canal.
    if (CAL[ID].active) CalibStop(ID, true);
    if (AT[ID].active)  AutoTuneStop(ID, true);

    CH[ID].active = true;
    CH[ID].startMs = millis();
    CH[ID].step = 0;
    CH[ID].stepStartMs = millis();
    CH[ID].curveLen = 0;
    for (int i = 0; i < CHAR_CURVE_LEN; i++)
    {
        CH[ID].pwmCurve[i] = 0;
        CH[ID].hzCurve[i] = 0.0f;
    }

    // Tomamos control del PWM: el loop ve IsCharActive y nos cede el
    // actuador (igual que en autotune).
    Sensor[ID].FlowEnabled = true;
    Sensor[ID].PidState = 0;
    Sensor[ID].PWM = 0;
    SetPWM(ID, 0); // rampeamos desde 0

    Serial.printf("[CHAR] start ID=%u steps=%u dwell=%ums\n",
                  ID, CHAR_STEPS, CHAR_DWELL_MS);
}

void CharStop(byte ID, bool cancelled)
{
    if (ID >= MaxProductCount) return;
    if (!CH[ID].active) return;

    CH[ID].active = false;
    SetPWM(ID, 0);
    Sensor[ID].PWM = 0;

    if (cancelled || CH[ID].curveLen < 2)
    {
        const char *err = cancelled ? "cancelled" : "insufficient_samples";
        publishCharResult(ID, false, 0, 0, 0.0f, 0.0f, nullptr, nullptr, 0, err);
        Serial.printf("[CHAR] stop ID=%u FAIL samples=%d cancelled=%d\n",
                      ID, CH[ID].curveLen, (int)cancelled);
        return;
    }

    // Derivar las métricas finales de la curva:
    //   pwm_min        = primer PWM donde hubo flujo detectable (Hz > 0.5)
    //   pwm_min_estable= primer PWM con flujo "útil" para PID (Hz > 5)
    //   hz_max         = máximo absoluto observado
    int pwmMin = 0;
    int pwmMinEstable = 0;
    float hzMax = 0.0f;
    for (int i = 0; i < CH[ID].curveLen; i++)
    {
        float hz = CH[ID].hzCurve[i];
        int pwm = CH[ID].pwmCurve[i];
        if (pwmMin == 0 && hz > 0.5f) pwmMin = pwm;
        if (pwmMinEstable == 0 && hz > 5.0f) pwmMinEstable = pwm;
        if (hz > hzMax) hzMax = hz;
    }

    // lmin_max = hz_max * 60 / meter_cal (pulsos por litro)
    float meterCal = (Sensor[ID].MeterCal > 0.001f) ? Sensor[ID].MeterCal : 1.0f;
    float lminMax = (hzMax * 60.0f) / meterCal;

    publishCharResult(ID, true, pwmMin, pwmMinEstable, hzMax, lminMax,
                      CH[ID].pwmCurve, CH[ID].hzCurve, CH[ID].curveLen, "");
    Serial.printf("[CHAR] OK ID=%u pwm_min=%d pwm_min_est=%d hz_max=%.2f lmin_max=%.2f\n",
                  ID, pwmMin, pwmMinEstable, hzMax, lminMax);
}

// Máquina de estados de la rampa: ejecutada cada tick desde main.loop().
// Avanza un paso cuando se cumple CHAR_DWELL_MS desde el cambio anterior,
// muestrea Hz al final del dwell (estado asentado), guarda (pwm, hz) y
// sube al siguiente PWM.
void RunChar(byte ID)
{
    if (ID >= MaxProductCount) return;
    if (!CH[ID].active) return;

    // Timeout duro: cualquier rampa de más de CHAR_TIMEOUT_MS se cierra con
    // lo que se haya logrado capturar.
    if (millis() - CH[ID].startMs > CHAR_TIMEOUT_MS)
    {
        Serial.println("[CHAR] timeout — auto-stop");
        CharStop(ID, false);
        return;
    }

    // Si el operario cierra todas las secciones a mitad de rampa, abortamos:
    // las muestras posteriores ya no son representativas.
    if (seccionesBits == 0)
    {
        Serial.println("[CHAR] secciones cerradas mid-rampa — cancel");
        CharStop(ID, true);
        return;
    }

    uint32_t now = millis();
    if (now - CH[ID].stepStartMs < CHAR_DWELL_MS) return;

    // Fin del dwell del paso actual → muestreamos Hz (asentado) y archivamos.
    int pwm = (int)(((uint32_t)CH[ID].step * 4095UL) / (uint32_t)(CHAR_STEPS - 1));
    if (pwm > 4095) pwm = 4095;
    float hz = Sensor[ID].UPM;

    if (CH[ID].curveLen < CHAR_CURVE_LEN)
    {
        CH[ID].pwmCurve[CH[ID].curveLen] = pwm;
        CH[ID].hzCurve[CH[ID].curveLen] = hz;
        CH[ID].curveLen++;
    }

    // Avanzar al próximo paso (o cerrar si era el último).
    CH[ID].step++;
    if (CH[ID].step >= CHAR_STEPS)
    {
        CharStop(ID, false);
        return;
    }

    int nextPwm = (int)(((uint32_t)CH[ID].step * 4095UL) / (uint32_t)(CHAR_STEPS - 1));
    if (nextPwm > 4095) nextPwm = 4095;
    Sensor[ID].PWM = nextPwm;
    SetPWM(ID, nextPwm);
    CH[ID].stepStartMs = now;
}

// --- Modo manual (búsqueda de pwm_min set/observar, bidireccional) -------
// El operario fija un PWM con signo y observa el caudal en vivo (status_live).
// Toma control exclusivo del PWM igual que Char/AutoTune: el PID no corre.
struct ManualState
{
    bool active;
    float value;        // PWM con signo aplicado (-4095..+4095)
    uint32_t lastCmdMs; // último manual_pwm recibido → failsafe comms-loss
    uint32_t startMs;   // inicio del modo → timeout duro
};
static ManualState MAN[MaxProductCount];

static const uint32_t MANUAL_COMMS_TIMEOUT_MS = 4000;   // 4s sin comando → stop
static const uint32_t MANUAL_HARD_TIMEOUT_MS  = 300000; // 5 min tope duro

bool IsManualActive(byte ID)
{
    if (ID >= MaxProductCount) return false;
    return MAN[ID].active;
}

void ManualSet(byte ID, float value)
{
    if (ID >= MaxProductCount) return;

    value = constrain(value, -4095.0f, 4095.0f);

    // Al entrar, cancelamos cualquier otro modo que pelee por el PWM del canal.
    if (!MAN[ID].active)
    {
        if (CAL[ID].active) CalibStop(ID, true);
        if (AT[ID].active)  AutoTuneStop(ID, true);
        if (CH[ID].active)  CharStop(ID, true);

        MAN[ID].active = true;
        MAN[ID].startMs = millis();
        Sensor[ID].FlowEnabled = true;
        Sensor[ID].PidState = 0; // off (no es el lazo PID)
        Serial.printf("[MANUAL] start ID=%u\n", ID);
    }

    MAN[ID].value = value;
    MAN[ID].lastCmdMs = millis();

    Sensor[ID].PWM = value; // reflejar en telemetría
    SetPWM(ID, value);      // >0 abre, <0 cierra
    Serial.printf("[MANUAL] ID=%u pwm=%.0f\n", ID, value);
}

void ManualStop(byte ID)
{
    if (ID >= MaxProductCount) return;
    if (!MAN[ID].active) return;

    MAN[ID].active = false;
    MAN[ID].value = 0;
    SetPWM(ID, 0);
    Sensor[ID].PWM = 0;
    Serial.printf("[MANUAL] stop ID=%u\n", ID);
}

void RunManual(byte ID)
{
    if (ID >= MaxProductCount) return;
    if (!MAN[ID].active) return;

    uint32_t now = millis();

    // Failsafe: si AOG deja de refrescar el comando, frenamos el motor.
    if (now - MAN[ID].lastCmdMs > MANUAL_COMMS_TIMEOUT_MS)
    {
        Serial.println("[MANUAL] comms-loss timeout — stop");
        ManualStop(ID);
        return;
    }
    // Tope duro por si quedó activo demasiado tiempo.
    if (now - MAN[ID].startMs > MANUAL_HARD_TIMEOUT_MS)
    {
        Serial.println("[MANUAL] hard timeout — stop");
        ManualStop(ID);
        return;
    }

    // Re-aplicar el PWM fijado.
    Sensor[ID].PWM = MAN[ID].value;
    SetPWM(ID, MAN[ID].value);
}
