#include <Arduino.h>
#include <Wire.h>
#include "Globals.h"
#include "Structs.h"
#include "Constants.h"
#include "Log.h"

extern bool ADSfound;
extern uint16_t PressureReading;

constexpr uint8_t ADS1115_Address = quantix::constants::ADS1115_ADDR;

void ReadAnalog()
{
    // Variables estáticas: mantienen su valor entre ejecuciones de la función
    // Esto crea una "Máquina de Estados" simple para no bloquear el loop.
    static int16_t Aread;
    static bool ConversionPending = false;

    if (ADSfound)
    {
        // El ADS1115 se usa para leer el sensor de presión (AIN0)
        // La lógica divide la operación en dos ciclos del loop:
        // 1. Solicitar lectura (Write Config)
        // 2. Leer resultado (Read Conversion)
        
        if (ConversionPending)
        {
            // Apuntamos al registro de conversión y leemos 2 bytes.
            Wire.beginTransmission(ADS1115_Address);
            Wire.write(0b00000000);
            uint8_t rc = Wire.endTransmission();
            if (rc != 0) {
                LOG_W("ads", "I2C err al apuntar conversión (%u)", (unsigned)rc);
                ConversionPending = false;
                return;
            }

            if (Wire.requestFrom((int)ADS1115_Address, 2) == 2)
            {
                Aread = (int16_t)(Wire.read() << 8 | Wire.read());
                if (Aread < 0) Aread = 0;
                PressureReading = (uint16_t)((uint16_t)Aread >> 1);
                ConversionPending = false;
            }
            else
            {
                LOG_W("ads", "I2C lectura incompleta");
                ConversionPending = false;
            }
        }
        else
        {
            // Single-shot: OS=1, MUX=AIN0, PGA=6.144V, MODE=1, DR=860SPS, comp off.
            Wire.beginTransmission(ADS1115_Address);
            Wire.write(0b00000001);
            Wire.write(0b11000001);
            Wire.write(0b11100011);
            uint8_t rc = Wire.endTransmission();
            if (rc != 0) {
                LOG_W("ads", "I2C err al configurar (%u)", (unsigned)rc);
                return;
            }
            ConversionPending = true;
        }
    }
}