#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include <PCF8574.h>
#include "Globals.h"
#include "Structs.h"

bool RelayStatus[16];

static uint8_t lastLoggedLo = 0xFF; // Para loguear solo cambios.

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
    // Si hay comunicación reciente, usar RelayLo/Hi que viene de MQTT.
    else if ((millis() - Sensor[0].CommTime < 4000) ||
             (MDL.SensorCount > 1 && (millis() - Sensor[1].CommTime < 4000)))
    {
        NewLo = RelayLo;
        NewHi = RelayHi;
    }
    // Timeout: sin comunicación → todo OFF.
    else
    {
        NewLo = 0;
        NewHi = 0;
    }

    // Log solo cuando cambia.
    if (NewLo != lastLoggedLo)
    {
        Serial.printf("⚡ CheckRelays: NewLo=0x%02X RelayLo=0x%02X RelayCtrl=%d CommAge=%lums\n",
            NewLo, RelayLo, MDL.RelayControl,
            millis() - Sensor[0].CommTime);
        lastLoggedLo = NewLo;
    }

    switch (MDL.RelayControl)
    {
    case 0: // None
        break;

    case 1: // GPIO directo
        for (int i = 0; i < 8; i++)
        {
            bool state = bitRead(NewLo, i);
            if (MDL.RelayControlPins[i] != NC)
                digitalWrite(MDL.RelayControlPins[i], MDL.InvertRelay ? !state : state);
        }
        break;

    case 5:
        // PCA9685 — lógica idéntica a SK21/RateControl.
        // Is3Wire=true:  1 pin por sección (IN1 only), 8 secciones max.
        // Is3Wire=false: 2 pines por sección (IN1+IN2, powered on/off), 8 secciones max.
        if (MDL.Is3Wire)
        {
            // 1 pin por sección: canal impar = sección.
            // SA1=pin1, SA2=pin3, SA3=pin5, SA4=pin7, etc.
            for (int i = 0; i < 8; i++)
            {
                bool state = bitRead(NewLo, i);
                if (RelayStatus[i] != state)
                {
                    uint8_t IOpin = (1 + i) * 2 - 1; // 1,3,5,7,9,11,13,15
                    if (state)
                        PWMServoDriver.setPWM(IOpin, 4096, 0);  // ON
                    else
                        PWMServoDriver.setPWM(IOpin, 0, 4096);  // OFF

                    RelayStatus[i] = state;

                    Serial.printf("🔌 SA%d (PCA ch%d) = %s\n", i + 1, IOpin, state ? "ON" : "OFF");
                }
            }
        }
        else
        {
            // 2 pines por sección: canal par=ON, canal impar=OFF.
            // SA1=pin0/pin1, SA2=pin2/pin3, etc.
            for (int i = 0; i < 8; i++)
            {
                bool state = bitRead(NewLo, i);
                if (RelayStatus[i] != state)
                {
                    uint8_t IOpin = i * 2;
                    if (state)
                    {
                        PWMServoDriver.setPWM(IOpin, 4096, 0);      // ON activo
                        PWMServoDriver.setPWM(IOpin + 1, 0, 4096);  // OFF inactivo
                    }
                    else
                    {
                        PWMServoDriver.setPWM(IOpin, 0, 4096);      // ON inactivo
                        PWMServoDriver.setPWM(IOpin + 1, 4096, 0);  // OFF activo
                    }

                    RelayStatus[i] = state;

                    Serial.printf("🔌 SA%d (PCA ch%d/%d) = %s\n",
                        i + 1, IOpin, IOpin + 1, state ? "ON" : "OFF");
                }
            }
        }
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
