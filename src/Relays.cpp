#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include "Globals.h"
#include "Structs.h"

bool RelayStatus[16];

void CheckRelays()
{
    uint8_t NewLo = 0;
    uint8_t NewHi = 0;

    if (WifiMasterOn)
    {
        if (millis() - WifiSwitchesTimer > 30000)   
        {
            WifiMasterOn = false;
        }
        else
        {
            for (int i = 0; i < 8; i++)
            {
                if (Button[i]) bitSet(NewLo, i);
                if (Button[i + 8]) bitSet(NewHi, i);
            }
        }
    }
    // Timeout de seguridad: Si no hay coms en 4 segundos, apagar todo
    else if ((millis() - Sensor[0].CommTime < 4000) || (MDL.SensorCount > 1 && (millis() - Sensor[1].CommTime < 4000)))
    {
        NewLo = RelayLo;
        NewHi = RelayHi;
    }

    // LOGICA DE HARDWARE
    switch (MDL.RelayControl)
    {
    case 0: // None
        break;

    case 1: // GPIO
        for (int i = 0; i < 8; i++)
        {
            bool state = bitRead(NewLo, i);
            // Pines definidos en MDL.RelayControlPins
            if (MDL.RelayControlPins[i] != NC) {
                 digitalWrite(MDL.RelayControlPins[i], MDL.InvertRelay ? !state : state);
            }
        }
        break;

    case 5: // PCA9685
        for (int i = 0; i < 8; i++)
        {
            bool state = bitRead(NewLo, i);
            if(RelayStatus[i] != state) {
                // PCA 0-15
                if(state) PWMServoDriver.setPWM(i, 4096, 0); // ON
                else PWMServoDriver.setPWM(i, 0, 4096);      // OFF
                RelayStatus[i] = state;
            }
        }
        // (Lógica similar para NewHi 8-15)
        break;

    case 6: // PCF8574
        if (PCF_found)
        {
            for (int i = 0; i < 8; i++)
            {
                bool state = bitRead(NewLo, i);
                PCF.write(i, MDL.InvertRelay ? !state : state);
            }
        }
        break;
    }
}