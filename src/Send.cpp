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

        // Solo enviamos MQTT
        for (int i = 0; i < MDL.SensorCount; i++) {
            sendMQTTStatus(i);
        }
    }
}