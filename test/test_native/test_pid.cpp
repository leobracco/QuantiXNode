#include <unity.h>
#include "core/PidCore.h"

using quantix::core::PidGains;
using quantix::core::PidState;
using quantix::core::computePidStep;

namespace {

PidGains defaultGains()
{
    PidGains g{};
    g.kp          = 2.5f;
    g.ki          = 1.5f;
    g.kd          = 0.0f;
    g.minOut      = 150.0f;
    g.maxOut      = 4095.0f;
    g.maxIntegral = 4095.0f;
    g.deadbandPct = 2.0f;
    g.slewRate    = 40.0f;
    return g;
}

}  // namespace

void setUp() {}
void tearDown() {}

// El caso que antes podía explotar: target==0 no debe dividir por cero,
// y la salida debe ser 0 sin tocar el integrador.
void test_target_zero_is_safe()
{
    PidGains g = defaultGains();
    PidState s{};
    float out = computePidStep(0.0f, 50.0f, 0.05f, g, s);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.integralSum);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.lastOut);
}

// Tras muchas iteraciones saturando contra maxOut, el integrador no debe
// exceder `maxIntegral`.
void test_anti_windup_does_not_explode()
{
    PidGains g = defaultGains();
    PidState s{};

    for (int i = 0; i < 1000; ++i) {
        computePidStep(200.0f, 0.0f, 0.05f, g, s);
    }
    TEST_ASSERT_TRUE(s.integralSum <= g.maxIntegral + 1e-3f);
    TEST_ASSERT_TRUE(s.integralSum >= -g.maxIntegral - 1e-3f);
}

// Dentro de la deadband, el error se anula: tras estabilizar, la salida
// se estaciona sin crecer indefinidamente.
void test_deadband_neutralizes_small_error()
{
    PidGains g = defaultGains();
    PidState s{};
    // Avanzamos la salida a algo > 0
    for (int i = 0; i < 40; ++i) {
        computePidStep(100.0f, 99.0f, 0.05f, g, s);
    }
    float before = s.integralSum;
    // Error relativo ~1% < deadband(2%): integrador debe mantenerse
    float out = computePidStep(100.0f, 99.0f, 0.05f, g, s);
    TEST_ASSERT_EQUAL_FLOAT(before, s.integralSum);
    TEST_ASSERT_TRUE(out >= g.minOut);
}

// El slew rate limita el cambio entre iteraciones.
void test_slew_rate_limits_change_per_step()
{
    PidGains g = defaultGains();
    g.slewRate = 10.0f;
    PidState s{};
    float prev = 0.0f;
    for (int i = 0; i < 30; ++i) {
        float out = computePidStep(500.0f, 0.0f, 0.05f, g, s);
        TEST_ASSERT_TRUE(out - prev <= g.slewRate + 1e-3f);
        prev = out;
    }
}

// Min-kick: si el cálculo devuelve un positivo por debajo de minOut,
// la salida real es minOut (para vencer inercia del motor).
void test_min_kick_applied_on_startup()
{
    PidGains g = defaultGains();
    g.kp = 0.01f; g.ki = 0.0f; // respuesta muy débil
    g.slewRate = 1e9f;          // sin limitar slew
    PidState s{};
    float out = computePidStep(10.0f, 0.0f, 0.05f, g, s);
    TEST_ASSERT_TRUE(out >= g.minOut);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_target_zero_is_safe);
    RUN_TEST(test_anti_windup_does_not_explode);
    RUN_TEST(test_deadband_neutralizes_small_error);
    RUN_TEST(test_slew_rate_limits_change_per_step);
    RUN_TEST(test_min_kick_applied_on_startup);
    return UNITY_END();
}
