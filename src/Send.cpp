#include <Arduino.h>
#include "Globals.h"
#include "Structs.h"
#include "Constants.h"

uint32_t SendLast = 0;

void SendComm()
{
    if (millis() - SendLast > quantix::constants::SEND_INTERVAL_MS)
    {
        SendLast = millis();

        for (int i = 0; i < MDL.SensorCount; i++) {
            sendMQTTStatus(i);
        }
    }
}