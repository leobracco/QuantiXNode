#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"

// Variables de estado del PID
uint32_t LastCheck[MaxProductCount];
float LastPWM[MaxProductCount] = { 0 };
float IntegralSum[MaxProductCount];

// Timer para el reporte de datos a MQTT (Gráfico)
uint32_t PidLogTimer[MaxProductCount] = {0};

// --- FUNCIÓN: RESETEAR ESTADO ---
// Se llama desde MQTT_Custom.cpp cuando cambias la config o pones manual
void ResetPIDState(byte ID)
{
    IntegralSum[ID] = 0.0f;  // Borrar memoria acumulada
    LastPWM[ID] = 0.0f;      // Borrar última salida
    Sensor[ID].PWM = 0;      // Borrar salida actual
    LastCheck[ID] = millis();// Reiniciar temporizador
    
    Serial.printf("[PID] Reset Estado Motor %d\n", ID);
}

// --- FUNCIÓN PRINCIPAL PID ---
// Se llama desde el loop principal
void PIDmotor(byte ID)
{
    // PROTECCIÓN DE SEGURIDAD:
    // Si por error la config vino con 0, forzamos 50ms para evitar división por cero o bucle infinito
    if (Sensor[ID].PIDtime < 10) Sensor[ID].PIDtime = 50;

    // Solo calculamos si el flujo está habilitado y hay un objetivo
    // O si estamos en modo Manual
    if (Sensor[ID].FlowEnabled && Sensor[ID].TargetUPM > 0)
    {
        // Verificar temporizador del PID
        if (millis() - LastCheck[ID] >= Sensor[ID].PIDtime)
        {
            // Calculamos dt asegurando que no sea 0 para que la integral funcione
            uint32_t now = millis();
            float dt = (now - LastCheck[ID]) / 1000.0f; 
            if (dt <= 0.0) dt = 0.05f; // Default a 50ms si dt da error
            
            LastCheck[ID] = now;

            // Recuperar valor anterior
            float Result = LastPWM[ID];

            // 1. CÁLCULO DE ERROR
            float RateError = Sensor[ID].TargetUPM - Sensor[ID].UPM;
            float errorPct = 0;
            if(Sensor[ID].TargetUPM > 0) 
                errorPct = (fabsf(RateError) / Sensor[ID].TargetUPM) * 100.0f;

            // 2. LÓGICA PID
            
            // A) Deadband (Zona Muerta)
            bool inDeadband = false;
            if (errorPct <= Sensor[ID].Deadband) { 
                RateError = 0; 
                inDeadband = true;
            }

            // B) Brake Point
            float BrakeFactor = 1.0f;
            if (Sensor[ID].BrakePoint > 0 && errorPct < Sensor[ID].BrakePoint) {
                BrakeFactor = errorPct / (float)Sensor[ID].BrakePoint;
                if (BrakeFactor < 0.2f) BrakeFactor = 0.2f; 
            }

            // C) Término Proporcional
            float pTerm = RateError * Sensor[ID].Kp * BrakeFactor;

            // D) Término Integral
            bool saturado = false;
            if (LastPWM[ID] >= Sensor[ID].MaxPWM && RateError > 0) saturado = true;
            if (LastPWM[ID] <= Sensor[ID].MinPWM && RateError < 0) saturado = true;

            // IMPORTANTE: Aquí es donde fallaba antes si dt era 0
            if (!saturado && !inDeadband) {
                IntegralSum[ID] += RateError * Sensor[ID].Ki * dt;
                
                float iLimit = Sensor[ID].MaxIntegral; 
                if (iLimit == 0) iLimit = 100.0f; 
                IntegralSum[ID] = constrain(IntegralSum[ID], -iLimit, iLimit);
            }

            float targetOutput = pTerm + IntegralSum[ID];

            // E) Slew Rate
            float change = targetOutput - Result;
            if (Sensor[ID].SlewRate > 0) {
                change = constrain(change, -(float)Sensor[ID].SlewRate, (float)Sensor[ID].SlewRate);
            }

            Result += change;

            // 3. LIMITACIÓN FINAL
            // Si el PID calcula menos que el mínimo, forzamos el mínimo para vencer inercia
            // (Siempre que no estemos en zona muerta con error 0)
            if (Result < Sensor[ID].MinPWM && !inDeadband) {
                Result = Sensor[ID].MinPWM;
            } else {
                 Result = constrain(Result, (float)Sensor[ID].MinPWM, (float)Sensor[ID].MaxPWM);
            }
           
            
            // Guardamos estado
            LastPWM[ID] = Result;
            Sensor[ID].PWM = Result; 
            
            // Debug Opcional (Descomentar solo si es necesario)
            // if(ID == 0) Serial.printf("T:%.1f A:%.1f PWM:%.0f\n", Sensor[ID].TargetUPM, Sensor[ID].UPM, Result);
        }
    }
    else
    {
        IntegralSum[ID] = 0;
        if (!Sensor[ID].AutoOn) {
            Sensor[ID].PWM = Sensor[ID].ManualAdjust;
            LastPWM[ID] = Sensor[ID].ManualAdjust;
        } else {
            Sensor[ID].PWM = 0;
            LastPWM[ID] = 0;
        }
    }
}
