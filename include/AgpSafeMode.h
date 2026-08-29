// ============================================================================
// AgpSafeMode.h
// Safe-mode persistido tras crashes consecutivos (Tanda 2 / doctrina industrial
// punto 6: "Modo seguro / timeouts").
//
// Lógica:
//   · NVS (Preferences) guarda `crash_count` y `last_clean_boot`.
//   · Al boot llamamos AgpSafeMode_begin() que:
//        - Lee `crash_count` previo.
//        - Mira `esp_reset_reason()`.
//        - Si fue crash (TASK_WDT / INT_WDT / PANIC / BROWNOUT / WDT) → incrementa
//          el contador y lo persiste.
//        - Si fue poweron limpio o sw_reset intencional → resetea a 0.
//   · `AgpSafeMode_isActive()` devuelve true si crash_count >= AGP_SAFE_MODE_THRESHOLD.
//     El firmware DEBE consultar esto antes de habilitar PWM/relays. En safe-mode:
//        - PWM = 0 forzado
//        - Relés OFF
//        - MQTT solo lee, NO ejecuta cmds peligrosos
//        - Acepta cmd "clear_safe_mode" del Hub para volver a operar.
//   · `AgpSafeMode_clear()` lo invoca el handler MQTT cuando recibe el cmd
//     correspondiente (con envelope+ack para auditoría).
//
// IMPORTANTE: este safe-mode NO compite con el `boot_reason` que ya publica
// el firmware en `/announcement` — son complementarios:
//   · boot_reason: indica qué causó el RESET ACTUAL (info instantánea).
//   · safe_mode:   indica que hubo VARIOS crashes seguidos (info persistente).
//                  Sólo se limpia con poweron limpio o cmd explícito.
// ============================================================================

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

#ifndef AGP_SAFE_MODE_THRESHOLD
#define AGP_SAFE_MODE_THRESHOLD 3
#endif

#ifndef AGP_SAFE_MODE_NS
#define AGP_SAFE_MODE_NS "agp_safe"
#endif

namespace _AgpSafeImpl {
    inline int &_crashCount() { static int n = 0; return n; }
    inline bool &_initialized() { static bool b = false; return b; }
}

/// Llamar UNA vez en setup(), antes de habilitar PWM. Devuelve el crash_count
/// actualizado (post-incremento si este boot fue por crash).
inline int AgpSafeMode_begin() {
    if (_AgpSafeImpl::_initialized()) return _AgpSafeImpl::_crashCount();
    _AgpSafeImpl::_initialized() = true;

    Preferences prefs;
    if (!prefs.begin(AGP_SAFE_MODE_NS, false)) {
        // NVS no abre — asumimos boot limpio para no bloquear operación.
        _AgpSafeImpl::_crashCount() = 0;
        return 0;
    }
    int prev = prefs.getInt("ccount", 0);
    esp_reset_reason_t r = esp_reset_reason();

    bool wasCrash =
        (r == ESP_RST_TASK_WDT) ||
        (r == ESP_RST_INT_WDT)  ||
        (r == ESP_RST_PANIC)    ||
        (r == ESP_RST_BROWNOUT) ||
        (r == ESP_RST_WDT);

    int newCount;
    if (wasCrash) {
        newCount = prev + 1;
    } else {
        // Poweron limpio o reset intencional → resetear el contador.
        // No tocamos NVS si ya estaba en 0 (ahorro de ciclos de escritura flash).
        newCount = 0;
    }

    if (newCount != prev) {
        prefs.putInt("ccount", newCount);
    }
    prefs.end();

    _AgpSafeImpl::_crashCount() = newCount;
    return newCount;
}

/// Cantidad de crashes consecutivos. 0 = todo bien.
inline int AgpSafeMode_crashCount() {
    return _AgpSafeImpl::_crashCount();
}

/// True si crash_count >= umbral — el firmware debe operar en modo degradado.
inline bool AgpSafeMode_isActive() {
    return _AgpSafeImpl::_crashCount() >= AGP_SAFE_MODE_THRESHOLD;
}

/// Reset manual desde Hub (cmd "clear_safe_mode") — el operario reconoce y
/// vuelve a operar normal. Persiste a 0 en NVS.
inline bool AgpSafeMode_clear() {
    Preferences prefs;
    if (!prefs.begin(AGP_SAFE_MODE_NS, false)) return false;
    prefs.putInt("ccount", 0);
    prefs.end();
    _AgpSafeImpl::_crashCount() = 0;
    return true;
}
