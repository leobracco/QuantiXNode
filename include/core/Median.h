#pragma once

#include <stdint.h>
#include <stddef.h>

namespace quantix {
namespace core {

// Mediana de un array sin tocar Arduino. Ordena una copia en stack
// usando insertion-sort (O(n^2) pero `n<=32` en este firmware).
// Devuelve 0 si `count == 0`.
inline uint32_t medianFromArray(const uint32_t* buf, size_t count)
{
    if (count == 0) return 0;
    // Tope razonable: la llamada del firmware nunca supera MaxSampleSize=20.
    constexpr size_t kMax = 32;
    if (count > kMax) count = kMax;

    uint32_t sorted[kMax];
    for (size_t i = 0; i < count; ++i) sorted[i] = buf[i];

    for (size_t i = 1; i < count; ++i) {
        uint32_t key = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = key;
    }

    if (count & 1u)
        return sorted[count / 2];
    return (sorted[count / 2 - 1] + sorted[count / 2]) / 2u;
}

}  // namespace core
}  // namespace quantix
