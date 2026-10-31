// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// Picasso MemoryMapLib retains ABL scanout at 0xa0000000 (35 MiB reserved).
// Never map
// arbitrary boot video as device memory. Geometry still comes from PE video.
static inline bool picasso_gop_range(uint64_t base, uint64_t stride,
    uint64_t width, uint64_t height, uint64_t depth, uint64_t rotation) {
    const uint64_t first = 0xa0000000, limit = 0xa2300000;
    return base >= first && base < limit && !(base & 3) &&
        width && width <= 16384 && height && height <= 16384 &&
        depth == 32 && !rotation && !(stride & 3) &&
        stride >= width * 4 && stride <= (limit - base) / height;
}

// CPU presentation supports linear BGRA only. '&BGA' is compressed storage
// on the original OS and cannot be treated as plain pixels without a GPU.
static inline bool picasso_gop_format(uint32_t format, uint32_t planes) {
    return format == 0x42475241 && planes == 0;
}

template <class Video> static inline bool picasso_gop_video(const Video &video) {
    if (!picasso_gop_range(video.v_baseAddr, video.v_rowBytes, video.v_width,
            video.v_height, video.v_depth, video.v_rotate) || video.v_offset ||
        (video.v_length && (video.v_length < video.v_rowBytes * video.v_height ||
            video.v_length > 0xa2300000 - video.v_baseAddr))) return false;
    const char expected[] = "BBBBBBBBGGGGGGGGRRRRRRRR";
    for (unsigned i = 0; i < sizeof(expected); ++i)
        if (video.v_pixelFormat[i] != expected[i]) return false;
    return true;
}
