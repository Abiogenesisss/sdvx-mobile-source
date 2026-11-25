#include "archive_texture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace sdvx {
namespace {

using Rgb = std::array<uint8_t, 3>;

Rgb rgb565(uint8_t lo, uint8_t hi) {
    const unsigned word = (static_cast<unsigned>(hi) << 8) | lo;
    const unsigned red = word >> 11;
    const unsigned green = (word >> 5) & 63;
    const unsigned blue = word & 31;
    return {
        static_cast<uint8_t>((red << 3) | (red >> 2)),
        static_cast<uint8_t>((green << 2) | (green >> 4)),
        static_cast<uint8_t>((blue << 3) | (blue >> 2)),
    };
}

}  // namespace

std::vector<uint8_t> decodeArchiveTexture(
    std::span<const uint8_t> pixels, int width, int height, bool dxt5) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    const auto columns = static_cast<std::size_t>(width);
    const auto rows = static_cast<std::size_t>(height);
    if (std::numeric_limits<std::size_t>::max() / 4 / columns < rows) {
        return {};
    }
    const std::size_t rgbaSize = columns * rows * 4;
    if (dxt5 ? pixels.size() % 2 != 0 : pixels.size() < rgbaSize) {
        return {};
    }
    std::vector<uint8_t> rgba(rgbaSize);
    if (!dxt5) {
        for (std::size_t byte = 0; byte < rgbaSize; byte += 4) {
            rgba[byte] = pixels[byte + 2];
            rgba[byte + 1] = pixels[byte + 1];
            rgba[byte + 2] = pixels[byte];
            rgba[byte + 3] = pixels[byte + 3];
        }
        return rgba;
    }

    for (std::size_t y = 0; y < rows; y += 4) {
        for (std::size_t x = 0; x < columns; x += 4) {
            std::array<uint8_t, 16> block{};
            const std::size_t blockBytes = std::min(block.size(), pixels.size());
            for (std::size_t byte = 0; byte < blockBytes; ++byte) {
                block[byte] = pixels[byte ^ 1];
            }
            pixels = pixels.subspan(blockBytes);

            // BC3 always has four RGB colors, regardless of endpoint order.
            std::array<Rgb, 4> colors{rgb565(block[8], block[9]), rgb565(block[10], block[11])};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const unsigned first = colors[0][channel];
                const unsigned second = colors[1][channel];
                colors[2][channel] = static_cast<uint8_t>((2 * first + second) / 3);
                colors[3][channel] = static_cast<uint8_t>((first + 2 * second) / 3);
            }

            std::array<uint8_t, 8> alpha{block[0], block[1], 0, 0, 0, 0, 0, 255};
            const unsigned alphaSteps = alpha[0] > alpha[1] ? 7 : 5;
            for (unsigned step = 1; step < alphaSteps; ++step) {
                alpha[step + 1] = static_cast<uint8_t>(
                    ((alphaSteps - step) * alpha[0] + step * alpha[1]) / alphaSteps);
            }
            uint64_t alphaIndices = 0;
            for (unsigned byte = 0; byte < 6; ++byte) {
                alphaIndices |= static_cast<uint64_t>(block[2 + byte]) << (8 * byte);
            }

            const std::size_t blockRows = std::min<std::size_t>(4, rows - y);
            const std::size_t blockColumns = std::min<std::size_t>(4, columns - x);
            for (std::size_t row = 0; row < blockRows; ++row) {
                for (std::size_t column = 0; column < blockColumns; ++column) {
                    const auto& rgb = colors[(block[12 + row] >> (column * 2)) & 3];
                    const auto opacity = alpha[(alphaIndices >> (3 * (row * 4 + column))) & 7];
                    auto* dest = rgba.data() + ((y + row) * columns + x + column) * 4;
                    std::copy(rgb.begin(), rgb.end(), dest);
                    dest[3] = opacity;
                }
            }
        }
    }
    return rgba;
}

}  // namespace sdvx
