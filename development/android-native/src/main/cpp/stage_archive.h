#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sdvx {

struct StageArchiveEntry {
    std::string archive;
    int64_t offset;
    size_t size;
    int width, height; // Zero for AFP, BSI and GE2D.
    bool dxt5, compressed;
};

const StageArchiveEntry* stageArchiveEntry(std::string_view virtualPath);
std::string readStageRange(const StageArchiveEntry& entry);
std::vector<uint8_t> readStagePixels(const StageArchiveEntry& entry);

} // namespace sdvx
