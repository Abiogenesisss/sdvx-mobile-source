#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sdvx {

// Decodes tightly packed BGRA8 or BC3 with each stored 16-bit word byte-swapped.
// BC3 follows ifstools: even-length short input is zero-padded, and extra bytes
// are ignored. Invalid dimensions, odd BC3 lengths, or short BGRA return empty.
std::vector<uint8_t> decodeArchiveTexture(
    std::span<const uint8_t> pixels, int width, int height, bool dxt5);

}  // namespace sdvx
