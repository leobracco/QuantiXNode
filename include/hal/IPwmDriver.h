#pragma once

#include <Arduino.h>

namespace quantix {
namespace hal {

// Interfaz abstracta para un driver PWM. Permite sustituir LEDC
// (salida ESP32 directa) por PCA9685 (I2C) sin tocar la capa de control.
class IPwmDriver {
public:
    virtual ~IPwmDriver() = default;
    virtual bool begin() = 0;

    // Escribe `value` (0..maxValue) en el canal lógico `channel`.
    virtual void setPwm(uint8_t channel, uint16_t value) = 0;

    // Valor máximo que acepta `setPwm` (típicamente 4095 en 12-bit).
    virtual uint16_t maxValue() const = 0;
};

}  // namespace hal
}  // namespace quantix
