#include "archive_lz.h"

namespace sdvx {
namespace {

uint32_t bigEndianWord(std::span<const uint8_t> bytes) {
    return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16)
        | (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
}

} // namespace

std::vector<uint8_t> unpackArchiveTexture(std::span<const uint8_t> stored, std::size_t expectedBytes,
                                          bool avslz) {
    if (!avslz) {
        if (stored.size() < expectedBytes) return {};
        return {stored.begin(), stored.end()};
    }

    constexpr std::size_t headerSize = 8;
    if (stored.size() < headerSize) return {};
    const auto payload = stored.subspan(headerSize);
    const auto compressedSize = bigEndianWord(stored.subspan(4, 4));
    if (payload.size() != compressedSize) {
        // ifstools' raw AVSLZ variant stores the last eight pixel bytes first.
        if (stored.size() < expectedBytes) return {};
        std::vector<uint8_t> pixels(payload.begin(), payload.end());
        pixels.insert(pixels.end(), stored.begin(), stored.begin() + headerSize);
        return pixels;
    }

    const auto decodedSize = bigEndianWord(stored.first(4));
    if (decodedSize < expectedBytes) return {};

    std::vector<uint8_t> pixels;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto flags = payload[offset++];
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (flags & (1u << bit)) {
                if (offset == payload.size() || pixels.size() == decodedSize) return {};
                pixels.push_back(payload[offset++]);
            } else {
                if (payload.size() - offset < 2) return {};
                const unsigned token = (unsigned{payload[offset]} << 8) | payload[offset + 1];
                offset += 2;
                const std::size_t distance = token >> 4;
                if (distance == 0) {
                    if (pixels.size() != decodedSize) return {};
                    return pixels;
                }

                const std::size_t length = (token & 0x0f) + 3;
                if (length > decodedSize - pixels.size()) return {};
                for (std::size_t byte = 0; byte < length; ++byte) {
                    // The history before the start of the output is zero-filled.
                    // Read one byte at a time so overlapping copies can reuse bytes just written.
                    pixels.push_back(distance > pixels.size() ? uint8_t{0} : pixels[pixels.size() - distance]);
                }
            }
        }
    }
    return {};
}

} // namespace sdvx
