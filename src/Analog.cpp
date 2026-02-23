#include <Arduino.h>
#include <Wire.h>
#include "Globals.h"
#include "Structs.h"

// --- VARIABLES EXTERNAS ---
// Estas variables se definen en main.cpp, pero las usamos aquí.
extern bool ADSfound;
extern uint16_t PressureReading;

// Dirección por defecto del ADS1115 (Pin ADDR a GND)
const uint8_t ADS1115_Address = 0x48; 

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
            // Intentar leer el valor si ya está listo
            Wire.beginTransmission(ADS1115_Address);
            Wire.write(0b00000000); // Apuntar al registro de Conversión (Puntero 0)
            Wire.endTransmission();

            // Pedimos 2 bytes
            if (Wire.requestFrom((int)ADS1115_Address, 2) == 2)
            {
                // Construimos el entero de 16 bits
                Aread = (int16_t)(Wire.read() << 8 | Wire.read());
                
                // Evitar valores negativos por ruido
                if (Aread < 0) Aread = 0;
                
                // Ajuste de escala específico de SK21 (Bit shift right 1 = dividir por 2)
                PressureReading = (uint16_t)((uint16_t)Aread >> 1);
                
                ConversionPending = false; // Lectura terminada, listos para la próxima
            }
        }
        else
        {
            // Iniciar nueva conversión (Single Shot Mode)
            Wire.beginTransmission(ADS1115_Address);
            Wire.write(0b00000001); // Apuntar al registro de Configuración (Puntero 1)

            // Escribir MSB del Config Register
            // Bit 15    = 1 (Iniciar conversión Single Shot)
            // Bits 14:12= 100 (MUX: AIN0 vs GND)
            // Bits 11:9 = 000 (Gain: 6.144V) -> SK21 usa 000 o 001 dependiendo de la versión,
            //               el snippet original usa 0b11000001 que implica GAIN variada.
            //               Ajustado al binario del código original:
            //               11000001: OS=1, MUX=100(AIN0), PGA=000(6.144V), MODE=1(Single)
            Wire.write(0b11000001); 

            // Escribir LSB del Config Register
            // Bits 7:5 = 111 (Data Rate: 860 SPS)
            // Bits 4:0 = 00011 (Comparador desactivado)
            Wire.write(0b11100011);

            Wire.endTransmission();

            ConversionPending = true;
        }
    }
}