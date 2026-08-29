#include "Globals.h"

#define MIN_PULSE_MICROS 250

// Spinlock multi-core para proteger los contadores compartidos ISR↔loop.
// noInterrupts() solo desactiva el CORE actual; si la ISR dispara en el
// otro core mientras GetUPM() copia el snapshot, hay race condition →
// contadores incorrectos y posible buffer overflow en Samples[][].
static portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;

IRAM_ATTR void PulseISR(uint8_t ID)
{
    // Guard de bounds — no asumir que el caller pasó un ID válido.
    if (ID >= MaxProductCount) return;

    uint32_t ReadTime = micros();
    uint32_t delta = ReadTime - ReadLast[ID];

    if (delta > MIN_PULSE_MICROS)
    {
        uint8_t size = Sensor[ID].PulseSampleSize;
        if (size == 0 || size > MaxSampleSize)
            size = 5;

        portENTER_CRITICAL_ISR(&pulseMux);
        PulseTime[ID] = delta;
        ReadLast[ID] = ReadTime;
        PulseCount[ID]++;

        Samples[ID][SamplesIndex[ID]] = PulseTime[ID];
        SamplesIndex[ID] = (SamplesIndex[ID] + 1) % size;

        if (SamplesCount[ID] < size)
            SamplesCount[ID]++;
        portEXIT_CRITICAL_ISR(&pulseMux);
    }
}

void IRAM_ATTR ISR_Sensor0() { PulseISR(0); }
void IRAM_ATTR ISR_Sensor1() { PulseISR(1); }

void GetUPM()
{
    for (int i = 0; i < MDL.SensorCount; i++)
    {
        if (PulseCount[i] > 0)
        {
            LastPulse[i] = millis();
            portENTER_CRITICAL(&pulseMux);
            Sensor[i].TotalPulses += PulseCount[i];
            PulseCount[i] = 0;
            uint16_t count = SamplesCount[i];
            uint32_t Snapshot[MaxSampleSize];
            for (uint16_t k = 0; k < count; k++)
                Snapshot[k] = Samples[i][k];
            portEXIT_CRITICAL(&pulseMux);

            uint32_t median = MedianFromArray(Snapshot, count);
            if (median > 0)
            {
                Sensor[i].Hz = 1000000.0f / (float)median;
                Sensor[i].UPM = Sensor[i].Hz;
            }
        }

        if (millis() - LastPulse[i] > 2000)
        {
            Sensor[i].UPM = 0;
            Sensor[i].Hz = 0;
            SamplesCount[i] = 0;
        }
    }
}