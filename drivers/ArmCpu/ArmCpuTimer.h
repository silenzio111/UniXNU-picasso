// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ARM_CPU_TIMER_H
#define ARM_CPU_TIMER_H
#include <stddef.h>
#include <stdint.h>

// ADT cells are little-endian, unlike Linux FDT cells. Picasso's DTB does not
// emit the property, so the fallback is its virtual-timer PPI; a malformed
// property must fail before MMIO.
static inline bool ArmCpuTimerIntid(const void *property, size_t bytes,
                                   uint32_t *result)
{
    if (!result) { return false; }
    if (!property) {
        if (bytes) { return false; }
        *result = 19;
        return true;
    }
    if (bytes != sizeof(uint32_t)) { return false; }
    const uint8_t *p = static_cast<const uint8_t *>(property);
    const uint32_t value = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    if (value < 16 || value > 31) { return false; }
    *result = value;
    return true;
}

// Only call with a validated PPI. Retain upstream's SGI enable policy.
static inline uint32_t ArmCpuSgiPpiEnable(uint32_t intid)
{
    return 0xffffU | (uint32_t(1) << intid);
}
#endif
