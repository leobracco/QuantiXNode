#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include "Globals.h"
#include "Structs.h"
#include "Constants.h"
#include "Log.h"

bool RelayStatus[16];

void CheckRelays()
{
    uint8_t NewLo = 0;
    uint8_t NewHi = 0;

    if (WifiMasterOn)
    {
        if (millis() - WifiSwitchesTimer > MDL.WifiTimeoutMs)
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
    // Timeout de seguridad: si no hay coms dentro de CommTimeoutMs, apagar todo.
    // Snapshot atómico de CommTime (volatile, puede escribirse desde callback MQTT).
    else
    {
        uint32_t comm0, comm1;
        noInterrupts();
        comm0 = Sensor[0].CommTime;
        comm1 = (MDL.SensorCount > 1) ? Sensor[1].CommTime : 0;
        interrupts();

        const uint32_t now = millis();
        bool commRecent0 = (now - comm0 < MDL.CommTimeoutMs);
        bool commRecent1 = (MDL.SensorCount > 1) && (now - comm1 < MDL.CommTimeoutMs);

        if (commRecent0 || commRecent1)
        {
            NewLo = RelayLo;
            NewHi = RelayHi;
        }
    }

    // Combinamos los 16 bits de salida (Low[0..7] | High[8..15]) en una palabra
    // para aplicar una sola vez por driver.
    const uint16_t relayMask = (uint16_t)NewLo | ((uint16_t)NewHi << 8);

    // LOGICA DE HARDWARE
    switch (MDL.RelayControl)
    {
    case 0: // None
        break;

    case 1: // GPIO (16 pines)
        for (int i = 0; i < 16; i++)
        {
            bool state = (relayMask >> i) & 0x1;
            if (MDL.RelayControlPins[i] != NC)
                digitalWrite(MDL.RelayControlPins[i], MDL.InvertRelay ? !state : state);
        }
        break;

    case 5: // PCA9685 (16 canales)
        for (int i = 0; i < 16; i++)
        {
            bool state = (relayMask >> i) & 0x1;
            if (RelayStatus[i] != state)
            {
                bool effective = MDL.InvertRelay ? !state : state;
                if (effective) PWMServoDriver.setPWM(i, 4096, 0);  // ON
                else           PWMServoDriver.setPWM(i, 0, 4096);  // OFF
                RelayStatus[i] = state;
            }
        }
        break;

    case 6: // PCF8574 (8 pines)
        if (PCF_found)
        {
            for (int i = 0; i < 8; i++)
            {
                bool state = (relayMask >> i) & 0x1;
                PCF.write(i, MDL.InvertRelay ? !state : state);
            }
        }
        break;

    default:
        LOG_W("relays", "RelayControl desconocido: %u", (unsigned)MDL.RelayControl);
        break;
    }
}