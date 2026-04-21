#pragma once

#include "IPwmDriver.h"
#include "Constants.h"

namespace quantix {
namespace hal {

// Adapter de `ledcSetup/ledcAttachPin/ledcWrite` del ESP32 Arduino core.
// Los canales LEDC se configuran externamente en el bootstrap; este wrapper
// solo expone la escritura en forma de IPwmDriver.
class LedcPwmDriver final : public IPwmDriver {
public:
    bool begin() override { return true; }

    void setPwm(uint8_t channel, uint16_t value) override {
        if (value > maxValue()) value = maxValue();
        ledcWrite(channel, value);
    }

    uint16_t maxValue() const override { return constants::PWM_MAX; }
};

}  // namespace hal
}  // namespace quantix
