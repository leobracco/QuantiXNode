#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"

uint32_t SendLast = 0;
const uint16_t SendTime = 200;

void SendComm()
{
    if (millis() - SendLast > SendTime)
    {
        SendLast = millis();

        // Enviamos el status individual de cada sensor/sección habilitada
        for (int i = 0; i < MDL.SensorCount; i++)
        {
            sendMQTTStatus(i); // Ahora la firma coincide: acepta 'i'
        }
    }
}