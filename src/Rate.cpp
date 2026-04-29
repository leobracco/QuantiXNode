#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"

// --- FILTRO ANTI-REBOTE ---
// Período mínimo entre pulsos en microsegundos.
// Inductivo 24 PPR a 350 RPM = 140 Hz → período 7142µs
// Filtro 2000µs = seguro hasta 500 Hz (24 PPR a 1250 RPM)
// Se cachea desde Sensor[].PulseMin al inicio para no leer struct en ISR.
static volatile uint32_t CachedPulseMin[MaxProductCount] = {2000, 2000};

// Variables de estado — sensor A (principal)
uint32_t LastPulse[MaxProductCount];
volatile uint32_t ReadLast[MaxProductCount];
volatile uint32_t PulseTime[MaxProductCount];
volatile uint16_t PulseCount[MaxProductCount];
volatile uint32_t TotalInterrupts[MaxProductCount] = {0};

// Variables de estado — sensor B (segundo sensor, opcional)
volatile uint32_t ReadLastB[MaxProductCount];
volatile uint32_t PulseTimeB[MaxProductCount];
volatile uint16_t PulseCountB[MaxProductCount];
bool HasSensorB[MaxProductCount] = {false, false};

// Cachear el filtro desde la config (llamar después de LoadData)
void CachePulseFilter()
{
    for (int i = 0; i < MaxProductCount; i++)
    {
        uint32_t val = Sensor[i].PulseMin;
        CachedPulseMin[i] = (val > 0 && val < 1000000) ? val : 2000;
        Serial.printf("[Rate] M%d PulseMin=%lu µs\n", i, CachedPulseMin[i]);
    }
}

// --- ISR: lo más corto posible, solo variables volátiles ---
IRAM_ATTR void PulseISR(uint8_t ID)
{
    uint32_t now = micros();
    uint32_t delta = now - ReadLast[ID];

    TotalInterrupts[ID]++;

    if (delta >= CachedPulseMin[ID])
    {
        PulseTime[ID] = delta;
        ReadLast[ID] = now;
        PulseCount[ID]++;
    }
}

void IRAM_ATTR ISR_Sensor0() { PulseISR(0); }
void IRAM_ATTR ISR_Sensor1() { PulseISR(1); }

// ISR sensor B — mismo motor, segundo sensor para promediar.
IRAM_ATTR void PulseISR_B(uint8_t ID)
{
    uint32_t now = micros();
    uint32_t delta = now - ReadLastB[ID];

    if (delta >= CachedPulseMin[ID])
    {
        PulseTimeB[ID] = delta;
        ReadLastB[ID] = now;
        PulseCountB[ID]++;
    }
}

void IRAM_ATTR ISR_Sensor0B() { PulseISR_B(0); }
void IRAM_ATTR ISR_Sensor1B() { PulseISR_B(1); }

// --- CÁLCULO: filtro exponencial + conteo de pulsos ---
void GetUPM()
{
    for (int i = 0; i < MDL.SensorCount; i++)
    {
        // Leer atómicamente los valores de ambas ISRs
        noInterrupts();
        uint16_t count = PulseCount[i];
        PulseCount[i] = 0;
        uint32_t period = PulseTime[i];
        uint16_t countB = PulseCountB[i];
        PulseCountB[i] = 0;
        uint32_t periodB = PulseTimeB[i];
        interrupts();

        // Sumar pulsos de ambos sensores.
        uint16_t totalCount = count + countB;

        if (totalCount > 0)
        {
            LastPulse[i] = millis();
            Sensor[i].TotalPulses += totalCount;

            // Promediar períodos si ambos sensores tienen lectura.
            uint32_t avgPeriod = period;
            if (HasSensorB[i] && period > 0 && periodB > 0)
                avgPeriod = (period + periodB) / 2;
            else if (period == 0 && periodB > 0)
                avgPeriod = periodB;

            if (avgPeriod > 0)
            {
                // Hz desde el período promedio entre pulsos.
                float hzNew = 1000000.0f / (float)avgPeriod;
                // Si hay dos sensores, la frecuencia real es el doble
                // (cada sensor ve la mitad de los pulsos del engranaje).
                if (HasSensorB[i]) hzNew *= 2.0f;

                // Filtro exponencial (EMA)
                float alpha = Sensor[i].Alpha;
                if (alpha <= 0.0f || alpha > 1.0f)
                    alpha = (Sensor[i].MotorType == MOTOR_HYDRAULIC) ? 0.2f : 0.4f;

                // Primera lectura: sin filtro
                if (Sensor[i].Hz < 0.01f)
                    Sensor[i].Hz = hzNew;
                else
                    Sensor[i].Hz = alpha * hzNew + (1.0f - alpha) * Sensor[i].Hz;

                Sensor[i].UPM = Sensor[i].Hz;

                // RPM para display
                if (Sensor[i].PulsesPerRev > 0)
                    Sensor[i].RPM = (Sensor[i].Hz * 60.0f) / (float)Sensor[i].PulsesPerRev;
            }
        }

        // Timeout
        uint32_t timeout = (Sensor[i].MotorType == MOTOR_HYDRAULIC) ? 1000 : 500;
        if (millis() - LastPulse[i] > timeout)
        {
            Sensor[i].UPM = 0;
            Sensor[i].Hz = 0;
            Sensor[i].RPM = 0;
        }
    }
}

// Reset limpio de contadores para calibración
void ResetPulseCounters(byte ID)
{
    noInterrupts();
    PulseCount[ID] = 0;
    PulseTime[ID] = 0;
    ReadLast[ID] = micros();
    interrupts();

    Sensor[ID].TotalPulses = 0;
    Sensor[ID].Hz = 0;
    Sensor[ID].UPM = 0;
    Sensor[ID].RPM = 0;
    LastPulse[ID] = millis();
}
