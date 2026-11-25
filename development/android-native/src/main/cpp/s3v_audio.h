#pragma once

#include <string>
#include <string_view>

namespace sdvx {
// A rate of zero keeps the source rate for playback through the mixer.
std::string decodeS3vWave(std::string_view source, int sampleRate);
}
