#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sdvx {

// Returns stored pixels with any trailing texture padding preserved.
// Returns an empty vector if the stream is malformed or shorter than expectedBytes.
std::vector<uint8_t> unpackArchiveTexture(std::span<const uint8_t> stored, std::size_t expectedBytes,
                                          bool avslz);

} // namespace sdvx
