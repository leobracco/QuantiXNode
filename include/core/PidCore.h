#pragma once

#include <stdint.h>
#include <cmath>

namespace quantix {
namespace core {

// Ganancias y límites de un lazo PID. No dependen de Arduino: se pueden
// instanciar en tests nativos.
struct PidGains {
    float kp;
    float ki;
    float kd;          // reservado (el lazo actual es PI; lo mantenemos por compatibilidad)
    float minOut;
    float maxOut;
    float maxIntegral;
    float deadbandPct;
    float slewRate;
};

// Estado persistente del controlador entre iteraciones.
struct PidState {
    float integralSum;
    float lastOut;
};

// Cálculo de un paso PI + anti-windup + deadband + min-kick + slew rate,
// completamente puro (sin millis ni Serial). `dt` en segundos.
//
// Invariantes:
//  - Si target < epsilon el error relativo se considera 0 (evita división
//    por cero y elimina ruido cuando el sistema está apagado).
//  - El integral solo se acumula si no estamos saturando contra un límite.
//  - La salida final queda en [minOut, maxOut] salvo cuando target<=0,
//    caso en que devolvemos 0.
inline float computePidStep(float target, float actual, float dt,
                            const PidGains& g, PidState& s)
{
    if (target <= 0.0f || dt <= 0.0f) {
        s.integralSum = 0.0f;
        s.lastOut = 0.0f;
        return 0.0f;
    }

    float error = target - actual;

    float errorPct = 0.0f;
    if (target >= 1e-3f)
        errorPct = (std::fabs(error) / target) * 100.0f;
    if (errorPct <= g.deadbandPct)
        error = 0.0f;

    float pTerm = error * g.kp;

    bool satHi = (s.lastOut >= g.maxOut && error > 0.0f);
    bool satLo = (s.lastOut <= g.minOut && error < 0.0f);

    if (!satHi && !satLo && error != 0.0f) {
        s.integralSum += error * g.ki * dt;
        float iLimit = g.maxIntegral > 0.0f ? g.maxIntegral : g.maxOut;
        if (s.integralSum > iLimit)  s.integralSum = iLimit;
        if (s.integralSum < -iLimit) s.integralSum = -iLimit;
    }

    float out = pTerm + s.integralSum;

    // Min-kick: si la salida pedida es positiva pero inferior al arranque,
    // forzamos el mínimo para vencer la inercia.
    if (out > 0.0f && out < g.minOut)
        out = g.minOut;

    // Slew rate: limitar el cambio entre iteraciones consecutivas.
    if (g.slewRate > 0.0f) {
        float maxChange = g.slewRate;
        if (out > s.lastOut + maxChange) out = s.lastOut + maxChange;
        if (out < s.lastOut - maxChange) out = s.lastOut - maxChange;
    }

    // Límite físico final.
    if (out > g.maxOut) out = g.maxOut;
    if (out < g.minOut) out = g.minOut;

    s.lastOut = out;
    return out;
}

}  // namespace core
}  // namespace quantix
