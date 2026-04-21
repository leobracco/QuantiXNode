#pragma once

#include <stdint.h>

namespace quantix {
namespace core {

// Tipo de actuador que controla el firmware. Reemplaza los `#define`
// antiguos (`StandardValve_ct`, `ComboClose_ct`...) por un enum tipado.
enum class ControlType : uint8_t {
    StandardValve = 0,
    ComboClose    = 1,
    Motor         = 2,
    Fan           = 3,
    TimedCombo    = 4,
};

}  // namespace core
}  // namespace quantix
