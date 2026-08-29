#include "Globals.h"

/**
 * Resetea las variables del PID para evitar saltos bruscos al arrancar
 */
void ResetPIDState(byte ID)
{
    if (ID >= MaxProductCount)
        return;

    IntegralSum[ID] = 0.0f;
    LastPWM[ID] = 0.0f;
    Sensor[ID].PWM = 0;
    LastCheck[ID] = millis();
}

/**
 * Cálculo del algoritmo PID para el motor/válvula
 */
void PIDmotor(byte ID)
{
    if (ID >= MaxProductCount)
        return;

    // Protección de tiempo mínimo
    if (Sensor[ID].PIDtime < 10)
        Sensor[ID].PIDtime = 10;

    // Si el flujo está desactivado o no hay objetivo, apagamos y salimos
    if (!Sensor[ID].FlowEnabled || Sensor[ID].TargetUPM <= 0)
    {
        IntegralSum[ID] = 0;
        LastPWM[ID] = 0;
        SetPWM(ID, 0);
        Sensor[ID].PWM = 0;        // sin esto, telemetría mostraba PWM fantasma
        Sensor[ID].PidState = 0; // off
        return;
    }

    // Sin pulsos pero hay setpoint: caudalímetro o bomba cortada.
    if (Sensor[ID].UPM <= 0.001f)
    {
        Sensor[ID].PidState = 1; // sin_pulsos
    }

    // Ejecutar solo si pasó el tiempo configurado (PIDtime)
    if (millis() - LastCheck[ID] >= Sensor[ID].PIDtime)
    {
        uint32_t dt_ms = millis() - LastCheck[ID];
        float dt = dt_ms / 1000.0f; // Delta tiempo en segundos
        LastCheck[ID] = millis();

        (void)dt; // sin término integral no se usa dt (ver nota de abajo)

        float error = Sensor[ID].TargetUPM - Sensor[ID].UPM; // en UPM (Hz)

        // ── CONTROL DE VÁLVULA MOTORIZADA (3-posiciones: abrir/mantener/cerrar) ──
        // La válvula es un actuador INTEGRADOR: el motor con puente H GIRA para
        // mover la válvula; el PWM es la VELOCIDAD del motor (con signo), NO el
        // caudal. La posición de la válvula = integral de esa velocidad. PWM 0 =
        // motor parado = la válvula MANTIENE el caudal.
        //   error > 0 (falta caudal) → abrir  → output > 0
        //   error < 0 (sobra caudal) → cerrar → output < 0
        //   |error| chico            → mantener → output 0
        //
        // NO se usa término integral: sobre un actuador integrador el integral
        // se satura mientras la válvula abre y, al llegar al objetivo, sigue
        // empujando a abrir (windup) → "se queda abriendo con los litros ya
        // correctos". El propio actuador integra, así que un proporcional con
        // banda muerta alcanza el objetivo sin offset acumulado.
        IntegralSum[ID] = 0.0f;

        // Banda muerta de CAUDAL (hold-band): si estamos suficientemente cerca
        // del objetivo, paramos el motor y la válvula mantiene su posición. Sin
        // esto el lazo nunca "suelta". 3% del objetivo con un piso absoluto chico
        // (evita perseguir ruido del caudalímetro a objetivos bajos).
        float holdBand = Sensor[ID].TargetUPM * 0.03f;
        if (holdBand < 0.05f) holdBand = 0.05f;

        float output;
        bool saturado = false;

        if (fabs(error) <= holdBand)
        {
            output = 0.0f; // mantener
        }
        else
        {
            // BANDA PROPORCIONAL: mapeamos la MAGNITUD del error al rango de
            // velocidad del motor [MinPWM, MaxPWM]. Cerca del objetivo (error
            // chico, recién fuera del hold-band) → MinPWM, gira lento y suave;
            // caudal lejos del objetivo (error grande) → MaxPWM, gira rápido.
            //
            // Por qué NO Kp·error directo: con Kp del autotune (~100) y el error
            // en Hz (objetivo de pocos Hz), Kp·error nunca llega a MinPWM (~2541),
            // así que el motor quedaba SIEMPRE pisado al mínimo y tardaba muchísimo
            // en corregir. Normalizando al objetivo el lazo se auto-escala sin
            // depender de las ganancias PID (que son de modelo bomba, no aplican
            // a una válvula motorizada integradora).
            //
            // "Error de velocidad plena" = el objetivo: si el caudal está a cero
            // (error == objetivo) vamos a MaxPWM. Piso 0.1 Hz para objetivos bajos.
            float fullErr = Sensor[ID].TargetUPM;
            if (fullErr < 0.1f) fullErr = 0.1f;

            float frac = (fabs(error) - holdBand) / (fullErr - holdBand);
            frac = constrain(frac, 0.0f, 1.0f);

            // Piso de PWM por sentido: abrir (error>0 → output>0) usa MinPWM,
            // cerrar (error<0 → output<0) usa MinPWMNeg. Cada sentido vence su
            // propia fricción mecánica, que suele ser distinta.
            float minFloor = (error > 0) ? (float)Sensor[ID].MinPWM
                                         : (float)Sensor[ID].MinPWMNeg;
            float mag = minFloor +
                        frac * (float)(Sensor[ID].MaxPWM - minFloor);

            output = (error > 0 ? +mag : -mag);

            saturado = (frac >= 1.0f);
        }

        // Aplicar a los actuadores (SetPWM>0 abre, <0 cierra, 0 mantiene)
        Sensor[ID].PWM = output;
        LastPWM[ID] = output;
        SetPWM(ID, output);

        // Estado del lazo para telemetría:
        //   1=sin_pulsos (no medimos caudal pese a setpoint) — prioritario
        //   2=saturado (pegado al techo con error)
        //   3=ok (regulando o manteniendo en banda muerta)
        if (Sensor[ID].UPM <= 0.001f)
            Sensor[ID].PidState = 1;
        else if (saturado)
            Sensor[ID].PidState = 2;
        else
            Sensor[ID].PidState = 3;
    }
}