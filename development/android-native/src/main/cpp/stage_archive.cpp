#include "stage_archive.h"
#include "archive_lz.h"
#include "archive_texture.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>

namespace sdvx {

const StageArchiveEntry* stageArchiveEntry(std::string_view path) {
    // Room and menu textures use the ordinary image loader. Delay the background index until play.
    if (path.ends_with(".json") || !(path.starts_with("stage_bg/") ||
            (path.starts_with("songs/") && path.find("/background/") != path.npos))) return nullptr;
    static const auto index = [] {
        size_t length = 0;
        std::unique_ptr<char, decltype(&SDL_free)> data(
            static_cast<char*>(SDL_LoadFile("graphics/stage-ifs.json", &length)), &SDL_free);
        if (!data) throw std::runtime_error("Missing background IFS index");
        const auto source = nlohmann::json::parse(data.get(), data.get() + length);
        if (source.at("version") != 1) throw std::runtime_error("Unsupported background IFS index");
        std::map<std::string, StageArchiveEntry, std::less<>> entries;
        for (const auto& [name, row] : source.at("entries").items()) {
            auto archive = row.at("archive").get<std::string>();
            const auto offset = row.at("offset").get<int64_t>();
            const auto size = row.at("size").get<int64_t>();
            const auto format = row.value("format", std::string{});
            const auto compression = row.value("compression", std::string{});
            const int width = row.value("width", 0), height = row.value("height", 0);
            if (!archive.starts_with("graphics/") || !archive.ends_with(".ifs") ||
                archive.find("/../") != archive.npos || archive.find_first_of("\\:") != archive.npos ||
                offset < 0 || size <= 0 || uint64_t(size) > std::numeric_limits<size_t>::max() ||
                (!format.empty() && ((format != "dxt5" && format != "argb8888rev") || width <= 0 || height <= 0 ||
                 uint64_t(width) * height > std::numeric_limits<size_t>::max() / 4)) ||
                (!compression.empty() && compression != "avslz" && compression != "none"))
                throw std::runtime_error("Invalid background IFS entry: " + name);
            entries.emplace(name, StageArchiveEntry{std::move(archive), offset, static_cast<size_t>(size),
                                                     width, height, format == "dxt5", compression == "avslz"});
        }
        return entries;
    }();
    const auto found = index.find(path);
    return found == index.end() ? nullptr : &found->second;
}

std::string readStageRange(const StageArchiveEntry& entry) {
    // Reuse one handle per worker. A scene may read many shapes from the same IFS;
    // separate handles avoid racing another worker's seek.
    using Handle = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>;
    thread_local Handle stream(nullptr, &SDL_CloseIO);
    thread_local std::string current;
    if (!stream || current != entry.archive) {
        stream.reset(SDL_IOFromFile(entry.archive.c_str(), "rb"));
        current = entry.archive;
    }
    if (!stream || SDL_SeekIO(stream.get(), entry.offset, SDL_IO_SEEK_SET) != entry.offset)
        throw std::runtime_error("Unable to open or seek background IFS: " + entry.archive);
    std::string bytes(entry.size, '\0');
    if (SDL_ReadIO(stream.get(), bytes.data(), bytes.size()) != bytes.size())
        throw std::runtime_error("Truncated background IFS: " + entry.archive);
    return bytes;
}

std::vector<uint8_t> readStagePixels(const StageArchiveEntry& entry) {
    const size_t w = entry.width, h = entry.height;
    const size_t needed = entry.dxt5 ? ((w + 3) / 4) * ((h + 3) / 4) * 16 : w * h * 4;
    std::vector<uint8_t> raw;
    {
        const auto bytes = readStageRange(entry);
        raw = unpackArchiveTexture({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, needed,
                                   entry.compressed);
    }
    if (raw.size() < needed) throw std::runtime_error("Invalid IFS texture payload: " + entry.archive);
    return decodeArchiveTexture({raw.data(), needed}, entry.width, entry.height, entry.dxt5);
}

} // namespace sdvx
