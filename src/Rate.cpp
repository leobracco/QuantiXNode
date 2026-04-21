#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"
#include "Constants.h"

// --- CONFIGURACIÓN DE FILTRO ---
// Filtro mínimo entre pulsos consecutivos para rechazar rebotes de encoder.
constexpr uint32_t MIN_PULSE_MICROS = quantix::constants::MIN_PULSE_MICROS;

// Variables compartidas con la ISR — deben ser volatile para que el
// compilador no cachee lecturas entre interrupciones.
volatile uint32_t LastPulse[MaxProductCount];
volatile uint32_t ReadLast[MaxProductCount];
volatile uint32_t PulseTime[MaxProductCount];

// Buffers
volatile uint32_t Samples[MaxProductCount][MaxSampleSize];
volatile uint16_t PulseCount[MaxProductCount];
volatile uint8_t SamplesCount[MaxProductCount];
volatile uint8_t SamplesIndex[MaxProductCount];

// Variable Debug
volatile uint32_t TotalInterrupts[2] = {0, 0};

// --- INTERRUPCIÓN (ISR) BLINDADA ---
IRAM_ATTR void PulseISR(uint8_t ID)
{
    TotalInterrupts[ID]++;

    uint32_t ReadTime = micros();
    uint32_t delta = ReadTime - ReadLast[ID];

    if (delta > MIN_PULSE_MICROS)
    {
        PulseTime[ID] = delta;
        ReadLast[ID] = ReadTime;

        PulseCount[ID]++;

        // --- PROTECCIÓN CONTRA DIVISIÓN POR CERO ---
        // Recuperamos el tamaño de muestra configurado
        uint8_t size = Sensor[ID].PulseSampleSize;

        // Si es 0 o mayor al máximo permitido, forzamos un valor seguro (5)
        // Esto evita el CRASH inmediato si la config está mal.
        if (size == 0 || size > MaxSampleSize)
            size = 5;

        Samples[ID][SamplesIndex[ID]] = PulseTime[ID];

        // Ahora es seguro usar el operador módulo (%)
        SamplesIndex[ID] = (SamplesIndex[ID] + 1) % size;

        if (SamplesCount[ID] < size)
            SamplesCount[ID]++;
    }
}

// Wrappers
void IRAM_ATTR ISR_Sensor0() { PulseISR(0); }
void IRAM_ATTR ISR_Sensor1() { PulseISR(1); }

// --- CÁLCULO ---
void GetUPM()
{
    for (int i = 0; i < MDL.SensorCount; i++)
    {
        uint16_t pendingCount;
        uint16_t samplesAvailable;
        uint32_t snapshotBuf[MaxSampleSize];

        noInterrupts();
        pendingCount = PulseCount[i];
        samplesAvailable = SamplesCount[i];
        if (pendingCount > 0)
        {
            Sensor[i].TotalPulses += pendingCount;
            PulseCount[i] = 0;
            for (uint16_t k = 0; k < samplesAvailable; k++)
                snapshotBuf[k] = Samples[i][k];
            LastPulse[i] = millis();
        }
        interrupts();

        if (pendingCount > 0)
        {
            uint32_t median = MedianFromArray(snapshotBuf, samplesAvailable);

            if (median > 0)
            {
                Sensor[i].Hz = 1000000.0f / (float)median;
                Sensor[i].UPM = Sensor[i].Hz;

                if (Sensor[i].DientesPorVuelta > 0)
                    Sensor[i].RPM = (Sensor[i].Hz * 60.0f) / (float)Sensor[i].DientesPorVuelta;
            }
        }

        noInterrupts();
        uint32_t lastPulseSnapshot = LastPulse[i];
        interrupts();

        if (millis() - lastPulseSnapshot > 2000)
        {
            Sensor[i].UPM = 0;
            Sensor[i].Hz = 0;
            Sensor[i].RPM = 0;
            noInterrupts();
            SamplesCount[i] = 0;
            interrupts();
        }
    }
}