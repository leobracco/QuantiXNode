#include "Globals.h"

// Resuelve qué modo de electroválvula usar para una sección dada.
//   true  → 3 cables (on/off simple sobre pinA)
//   false → 2 cables (inversión de polaridad pinA/pinB)
// Si el override per-sección es -1 (default), cae al CFG.Is3Wire global.
static inline bool sectionIs3Wire(int i)
{
    if (i < 0 || i >= 10) return CFG.Is3Wire;
    int8_t ov = CFG.SectionIs3Wire[i];
    if (ov == 0) return false;
    if (ov == 1) return true;
    return CFG.Is3Wire;
}

void processValves(uint16_t bits)
{
    // OVERRIDE manual desde el portal web — bits seteados en sectionForceOff
    // fuerzan esa sección APAGADA aunque AOG la mande prendida en el target.
    // Es un kill-switch operativo para tirar una sección rápido sin pasar
    // por AOG (p. ej. para mantenimiento o sección rota).
    bits &= ~sectionForceOff;

    seccionesBits = bits;

    // 1. Válvula Master: mayor potencia (usualmente GPIO 14 o pin dedicado).
    //    Se enciende sólo si hay alguna sección abierta Y dosis objetivo > 0.
    bool masterActive = (bits > 0 && CFG.dosisTarget > 0);
    if (CFG.MasterPin != NC)
    {
        digitalWrite(CFG.MasterPin, CFG.InvertRelay ? !masterActive : masterActive);
    }

    // 2. Control de secciones — hasta 10 (consistente con SectionPins[10][2]).
    //    Antes era hasta 8 por mover bits sobre lowByte; ahora leemos cada
    //    bit directo del uint16_t.
    for (int i = 0; i < 10; i++)
    {
        bool state = (bits >> i) & 0x1;

        if (sectionIs3Wire(i))
        {
            // --- 3 CABLES: on/off simple ---
            // Cable común a +V, cable común a GND, cable de control que
            // sale "positivo" (HIGH) para abrir o "nada" (LOW) para cerrar.
            // pinA = línea de control. pinB no se usa en este modo.
            uint8_t pin = CFG.SectionPins[i][0];
            if (pin != NC)
                digitalWrite(pin, CFG.InvertRelay ? !state : state);
        }
        else
        {
            // --- 2 CABLES: inversión de polaridad (motorizada / latching) ---
            // Driver tipo puente-H sobre pinA/pinB. Abrir = pinA+ pinB-,
            // cerrar = pinA- pinB+. InvertRelay no aplica acá: es polaridad,
            // no lógica de activación.
            uint8_t pinA = CFG.SectionPins[i][0];
            uint8_t pinB = CFG.SectionPins[i][1];
            if (pinA != NC && pinB != NC)
            {
                if (state)
                { // Abrir
                    digitalWrite(pinA, HIGH);
                    digitalWrite(pinB, LOW);
                }
                else
                { // Cerrar
                    digitalWrite(pinA, LOW);
                    digitalWrite(pinB, HIGH);
                }
            }
        }
    }
}

void CheckRelays()
{
    // Timeout de seguridad: 4 segundos sin recibir datos del Bridge.
    // Cortamos todo para que la pulverizadora no quede pulsando sin control
    // si AOG/AgIO se cae o la red MQTT se corta.
    if (millis() - Sensor[0].CommTime > 4000)
    {
        processValves(0);
    }
}
