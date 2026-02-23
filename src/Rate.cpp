#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"

// --- CONFIGURACIÓN DE FILTRO ---
#define MIN_PULSE_MICROS 250 

// Variables
uint32_t LastPulse[MaxProductCount];
uint32_t ReadLast[MaxProductCount];
uint32_t PulseTime[MaxProductCount];

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
        if (size == 0 || size > MaxSampleSize) size = 5; 

        Samples[ID][SamplesIndex[ID]] = PulseTime[ID];
        
        // Ahora es seguro usar el operador módulo (%)
        SamplesIndex[ID] = (SamplesIndex[ID] + 1) % size;
        
        if (SamplesCount[ID] < size) SamplesCount[ID]++;
    }
}

// Wrappers
void IRAM_ATTR ISR_Sensor0() { PulseISR(0); }
void IRAM_ATTR ISR_Sensor1() { PulseISR(1); }

// --- CÁLCULO ---
void GetUPM()
{
    // DEBUG: Imprimir estado cada 1 segundo
    static uint32_t DebugTimer = 0;
    bool Imprimir = (millis() - DebugTimer > 1000);
    if (Imprimir) DebugTimer = millis();

    for (int i = 0; i < MDL.SensorCount; i++)
    {
        if (Imprimir==58) {
            Serial.printf("[DEBUG S%d] Ints: %u | BUFFER: %d | DeltaUs: %u | Pin: %d\n", 
                i, TotalInterrupts[i], PulseCount[i], PulseTime[i], digitalRead(Sensor[i].FlowPin));
        }

        if (PulseCount[i] > 0)
        {
            LastPulse[i] = millis();

            noInterrupts();
            Sensor[i].TotalPulses += PulseCount[i];
            PulseCount[i] = 0; 
            
            uint16_t count = SamplesCount[i];
            uint32_t Snapshot[MaxSampleSize];
            for (uint16_t k = 0; k < count; k++) Snapshot[k] = Samples[i][k];
            interrupts();

            uint32_t median = MedianFromArray(Snapshot, count);

            if (median > 0)
            {
                Sensor[i].Hz = 1000000.0f / (float)median;
                
                if (Sensor[i].MeterCal > 0)
                    Sensor[i].UPM = (Sensor[i].Hz * 60.0f) / Sensor[i].MeterCal;
                else
                    Sensor[i].UPM = 0;
                if (Sensor[i].PulsesPerRev > 0)
                Sensor[i].RPM = (Sensor[i].Hz * 60.0f) / (float)Sensor[i].PulsesPerRev;
            else
                Sensor[i].RPM = 0;
            }
        }
        
        uint32_t timeout = Sensor[i].PulseMax / 1000;
        if(timeout == 0) timeout = 2000; 

        if (millis() - LastPulse[i] > timeout)
        {
            Sensor[i].UPM = 0;
            Sensor[i].Hz = 0;
            Sensor[i].RPM = 0; // RPM a 0
            SamplesCount[i] = 0;
        }
    }
}
