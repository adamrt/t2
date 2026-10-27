#pragma once

#include <stddef.h>
#include <stdint.h>

// Decode one character, consuming an invalid or incomplete leading byte alone.
static inline uint32_t
utf8_next(const uint8_t **text, const uint8_t *end)
{
    const uint8_t *bytes = *text;
    uint32_t lead = *bytes, continuations = lead >= 0xf0 && lead < 0xf5   ? 3
                                            : lead >= 0xe0                ? 2
                                            : lead >= 0xc2 && lead < 0xe0 ? 1
                                                                          : 0;
    *text = bytes + 1;
    if (lead < 0x80)
        return lead;
    if (continuations == 0 || lead >= 0xf5 || (ptrdiff_t)continuations >= end - bytes)
        return 0xfffd;
    uint32_t character = lead & (0x3fu >> continuations);
    for (uint32_t k = 1; k <= continuations; k++) {
        if ((bytes[k] & 0xc0) != 0x80)
            return 0xfffd;
        character = character << 6 | (bytes[k] & 0x3fu);
    }
    *text = bytes + continuations + 1;
    return character;
}
