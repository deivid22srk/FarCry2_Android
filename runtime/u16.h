/* Guest wide strings are UTF-16: 2-byte units, terminated by a 2-byte 0x0000.
 *
 * The host's wchar_t is 4 bytes on Linux and Android, so the C library's wcs*
 * functions are wrong for guest data: wcslen() counts half the units (and runs
 * past the end when adjacent memory is non-zero), content, and show up in
 * swprintf()/towlower() which write 4 bytes per element into a 2-byte buffer.
 * Everything the runtime does on a guest wide string must go through these.
 *
 * The pointers are host pointers into MEM (GPTR(guest_offset)); the helpers use
 * 2-byte little-endian units, matching the guest. */
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint16_t u16_at(const void *p, uint32_t i) {
    uint16_t v;
    memcpy(&v, (const uint8_t *)p + 2u * i, 2);
    return v;
}
static inline void u16_put(void *p, uint32_t i, uint16_t v) {
    memcpy((uint8_t *)p + 2u * i, &v, 2);
}
static inline uint32_t u16_len(const void *p) {
    uint32_t n = 0;
    if (!p) return 0;
    while (u16_at(p, n)) n++;
    return n;
}
static inline int u16_cmp_n(const void *a, const void *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = u16_at(a, i), y = u16_at(b, i);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
