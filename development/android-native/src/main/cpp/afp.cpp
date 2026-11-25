#include "afp.h"
#include "afp_movie.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace sdvx {
namespace {
using Json = nlohmann::json;
using Mask = std::vector<Vec2>;
struct Command {
    size_t texture;
    std::string binding;
    std::array<float, 9> matrix;
    Color multiply, add;
    Blend blend;
    std::vector<Mask> masks;
    Rect sourceRect{};
    Rect geometry{};
    bool nearest = false;
    std::array<float, 3> hsl{};
};
struct Frames {
    std::vector<std::vector<Command>> frames;
    std::vector<size_t> offsets;
    mutable std::vector<Command> decoded;
    mutable size_t decodedFrame = size_t(-1);

    size_t count() const { return offsets.empty() ? frames.size() : offsets.size(); }
};
struct Layer : Frames { size_t repeatFrom = 0; };
struct Clip : Frames {
    double fps = 0;
    Vec2 viewport{};
    std::vector<Layer> layers;
    std::vector<size_t> textures;
};
struct Timeline {
    struct Region { Rect rect, geometry; bool nearest; };
    std::vector<std::string> textures;
    std::vector<Region> regions;
    std::vector<std::string> paths;
    std::unordered_map<std::string, Clip> clips;
    std::string binary;
    bool colorShift = false;
};

struct OriginalClip {
    AfpMovie movie;
    size_t start, count, preroll;
    bool continuous, holdRoot;
    std::vector<std::pair<std::string, std::string>> cues;
    std::optional<size_t> elapsed;

    OriginalClip(const AfpMovie& source, const Json& data)
        : movie(source.fork()), start(frame(data.at("start"))),
          count(data.contains("end") ? frame(data.at("end")) - start : data.value("frames", movie.frames() - start)),
          preroll(data.contains("preroll") ? frame(data.at("preroll")) : start),
          continuous(data.value("continuous", false)), holdRoot(data.value("hold_root", false)),
          cues(data.value("cue", decltype(cues){})) {
        if (!count || (count > movie.frames() - start && !(holdRoot && continuous)) || preroll > start)
            throw std::runtime_error("Invalid AFP clip range");
    }

    size_t frame(const Json& value) const {
        if (value.is_number_unsigned()) {
            const auto index = value.get<size_t>();
            if (index <= movie.frames()) return index;
        } else if (value.is_string()) {
            if (auto index = movie.label(value.get_ref<const std::string&>())) return *index;
        }
        throw std::runtime_error("Unknown original AFP frame " + value.dump());
    }

    void draw(Render& render, double ms, bool loop, Rect bounds, const Bindings& bindings,
              const std::array<Render::Projected, 3>* plane, float opacity) {
        size_t tick = std::floor(std::max(0.0, ms) * movie.fps() / 1000 + 1e-7);
        if (!loop) tick = std::min(tick, count - 1);
        else if (!continuous) tick %= count;
        if (!elapsed || tick < *elapsed) {
            movie.start(preroll, -1000. * (start - preroll) / movie.fps());
            if (holdRoot) movie.holdRoot();
            for (const auto& [path, label] : cues) movie.cue(path, label, 0);
            if (!cues.empty()) movie.applyCues();
        }
        elapsed = tick;
        movie.advance((tick + .0001) * 1000 / movie.fps());
        movie.draw(render, bounds, bindings, plane, opacity);
    }
};

std::string clipKey(std::string_view animation, std::string_view clip) {
    return std::string(animation) + ":" + std::string(clip);
}

Blend blendMode(int value) {
    if (value == 8) return Blend::Add;
    if (value == 3) return Blend::Multiply;
    return Blend::Alpha;
}

Color color(const Json& data) {
    return {data.at("r"), data.at("g"), data.at("b"), data.at("a")};
}

Timeline jsonTimeline(std::string_view data) {
    auto source = Json::parse(data);
    Timeline timeline;
    std::unordered_map<std::string, size_t> names;
    for (const auto& [animation, animationData] : source.items()) {
        Vec2 viewport{animationData.at("viewport").at(0), animationData.at("viewport").at(1)};
        double fps = animationData.at("fps");
        for (const auto& [label, clipData] : animationData.at("clips").items()) {
            auto& clip = timeline.clips[clipKey(animation, label)];
            clip.fps = fps;
            clip.viewport = viewport;
            auto readFrames = [&](const Json& frames, std::vector<std::vector<Command>>& targetFrames) {
                for (const auto& frame : frames) {
                    auto& target = targetFrames.emplace_back();
                    target.reserve(frame.size());
                    for (const auto& row : frame) {
                        const auto& texture = row.at("texture").get_ref<const std::string&>();
                        auto [name, inserted] = names.try_emplace(texture, timeline.textures.size());
                        if (inserted) timeline.textures.push_back(texture);
                        const auto& m = row.at("matrix");
                        Command command{name->second, row.value("binding", std::string{}),
                            {m.at("a"), m.at("c"), m.at("tx"), m.at("b"), m.at("d"), m.at("ty"),
                             m.value("p0", 0.0f), m.value("p1", 0.0f), 1},
                            color(row.at("multiply")), color(row.at("add")), blendMode(row.at("blend")), {}};
                        if (auto rect = row.find("source_rect"); rect != row.end())
                            command.sourceRect = {rect->at(0), rect->at(1), rect->at(2), rect->at(3)};
                        if (auto rect = row.find("geometry"); rect != row.end())
                            command.geometry = {rect->at(0), rect->at(1), rect->at(2), rect->at(3)};
                        command.nearest = row.value("filter", std::string{}) == "nearest";
                        if (auto masks = row.find("masks"); masks != row.end()) {
                            for (const auto& mask : *masks) {
                                auto& points = command.masks.emplace_back();
                                points.reserve(mask.size());
                                for (const auto& point : mask) points.push_back({point.at(0), point.at(1)});
                            }
                        }
                        target.push_back(std::move(command));
                    }
                }
            };
            if (auto layers = clipData.find("layers"); layers != clipData.end()) {
                for (const auto& sourceLayer : *layers) {
                    auto& layer = clip.layers.emplace_back();
                    layer.repeatFrom = sourceLayer.at("repeat_from");
                    readFrames(sourceLayer.at("frames"), layer.frames);
                    if (layer.repeatFrom >= layer.frames.size())
                        throw std::runtime_error("Invalid AFP layer repeat frame");
                }
            } else {
                readFrames(clipData.at("frames"), clip.frames);
            }
        }
    }
    return timeline;
}

struct Reader {
    std::string_view data;
    size_t offset = 0;
    std::string_view take(size_t length) {
        if (length > data.size() - offset) throw std::runtime_error("Truncated APB1 timeline");
        auto bytes = data.substr(offset, length);
        offset += length;
        return bytes;
    }
    uint32_t integer(size_t length) {
        uint32_t value = 0;
        for (unsigned char byte : take(length)) value = (value << 8) | byte;
        return value;
    }
    float real() { return std::bit_cast<float>(integer(4)); }
    Color tint() { return {real(), real(), real(), real()}; }
    std::string text() { return std::string(take(integer(2))); }
};

std::string inflateGzip(std::string_view bytes) {
    z_stream stream{};
    if (inflateInit2(&stream, 31) != Z_OK) throw std::runtime_error("Could not initialize AFP gzip reader");
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
    stream.avail_in = bytes.size();
    std::string result;
    std::array<char, 32768> buffer;
    int status;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = buffer.size();
        status = inflate(&stream, Z_NO_FLUSH);
        result.append(buffer.data(), buffer.size() - stream.avail_out);
    } while (status == Z_OK);
    inflateEnd(&stream);
    if (status != Z_STREAM_END) throw std::runtime_error("Could not decompress AFP timeline");
    return result;
}

Timeline binaryTimeline(std::string_view data) {
    Timeline timeline;
    timeline.binary = inflateGzip(data);
    Reader reader{timeline.binary};
    const auto version = reader.integer(4);
    if (version < 0x41504231 || version > 0x41504234) throw std::runtime_error("Unsupported AFP timeline format");
    timeline.colorShift = version == 0x41504234;
    auto textures = reader.integer(2);
    for (uint32_t i = 0; i < textures; ++i) {
        timeline.textures.push_back(reader.text());
        if (version >= 0x41504232)
            timeline.regions.push_back({{reader.real(),reader.real(),reader.real(),reader.real()},
                                       {reader.real(),reader.real(),reader.real(),reader.real()},reader.integer(1)!=0});
    }
    auto clips = reader.integer(2);
    for (uint32_t i = 0; i < clips; ++i) {
        auto& clip = timeline.clips[reader.text()];
        clip.fps = reader.real();
        clip.viewport = version >= 0x41504232 ? Vec2{reader.real(),reader.real()} : Vec2{Width,Height};
        std::vector<bool> used(timeline.textures.size());
        auto readFrames = [&](Frames& frames) {
            auto count = reader.integer(2);
            frames.offsets.reserve(count);
            for (uint32_t f = 0; f < count; ++f) {
                frames.offsets.push_back(reader.offset);
                auto draws = reader.integer(2);
                for (uint32_t d = 0; d < draws; ++d) {
                    const auto texture = reader.integer(2);
                    if (!used.at(texture)) {
                        used[texture] = true;
                        clip.textures.push_back(texture);
                    }
                    reader.take(9 * 4 + 8 * 4 + 1);
                    auto masks = reader.integer(1);
                    for (uint32_t m = 0; m < masks; ++m) reader.take(reader.integer(1) * 8);
                    if (timeline.colorShift && reader.integer(1)) reader.take(3 * 4);
                }
            }
        };
        if (version >= 0x41504233) {
            const auto layers = reader.integer(2);
            for (uint32_t j = 0; j < layers; ++j) {
                auto& layer = clip.layers.emplace_back();
                layer.repeatFrom = reader.integer(2);
                readFrames(layer);
                if (layer.repeatFrom >= layer.count()) throw std::runtime_error("Invalid AFP layer repeat frame");
            }
        } else readFrames(clip);
    }
    return timeline;
}

Command binaryCommand(Reader& reader, bool colorShift) {
    Command command{};
    command.texture = reader.integer(2);
    for (float& value : command.matrix) value = reader.real();
    command.multiply = reader.tint();
    command.add = reader.tint();
    command.blend = blendMode(reader.integer(1));
    auto masks = reader.integer(1);
    for (uint32_t m = 0; m < masks; ++m) {
        auto& mask = command.masks.emplace_back();
        auto points = reader.integer(1);
        mask.reserve(points);
        for (uint32_t p = 0; p < points; ++p) mask.push_back({reader.real(), reader.real()});
    }
    if (colorShift && reader.integer(1))
        for (auto& channel : command.hsl) channel = reader.real();
    return command;
}
}

struct AfpLibrary::Impl {
    using Original = std::unordered_map<std::string, OriginalClip>;
    std::unordered_map<std::string, Original> originals;
    std::unordered_map<std::string, Timeline> timelines;
    std::vector<std::string> queued;
    std::future<std::unique_ptr<Impl>> parsing;

    Timeline& load(const std::string& path) {
        auto [it, inserted] = timelines.try_emplace(path);
        if (inserted) {
            // Keep existing downloaded crew usable until their original AFP bundle is installed.
            auto bytes = path.starts_with("crew/") ? readExternalAsset(path) : std::string{};
            if (!bytes.empty()) {
                auto& timeline = it->second;
                timeline = path.ends_with(".dat") ? binaryTimeline(bytes) : jsonTimeline(bytes);
                const auto folder = path.substr(0, path.rfind('/') + 1);
                for (const auto& texture : timeline.textures) timeline.paths.push_back(folder + texture + ".png");
            }
        }
        return it->second;
    }

    Original& loadOriginal(const std::string& folder) {
        auto [it, inserted] = originals.try_emplace(folder);
        if (inserted) {
            const auto bytes = readAsset(folder + "/afp-library.json");
            if (!bytes.empty()) {
                const auto data = Json::parse(bytes);
                for (const auto& [name, movie] : data.items()) {
                    const auto& clips = movie.at("clips");
                    if (clips.empty()) continue;
                    const auto sourceName = movie.value("source", name);
                    AfpMovie source(folder, sourceName);
                    const auto options = movie.value("options", Json::object());
                    source.configure(options);
                    for (const auto& [label, clip] : clips.items()) {
                        auto [playback, _] = it->second.try_emplace(clipKey(name, label), source, clip);
                        if (const auto overrides = clip.find("options"); overrides != clip.end()) {
                            auto configured = options;
                            configured.update(*overrides);
                            playback->second.movie.configure(configured);
                        }
                    }
                }
            }
        }
        return it->second;
    }

    void loadFolder(const std::string& folder) {
        if (loadOriginal(folder).empty() && folder.starts_with("crew/")) {
            for (const auto extension : {".dat", ".json"}) load(folder + "/timeline" + extension);
        }
    }

    OriginalClip* original(std::string_view folder, std::string_view animation, std::string_view label) {
        auto& clips = loadOriginal(std::string(folder));
        if (const auto it = clips.find(clipKey(animation, label)); it != clips.end()) return &it->second;
        return nullptr;
    }

    std::pair<const Timeline*, const Clip*> get(std::string_view folder, std::string_view animation, std::string_view label) {
        if (!folder.starts_with("crew/")) return {nullptr, nullptr};
        std::string base = std::string(folder) + "/timeline";
        const std::string key = clipKey(animation, label);
        for (const auto extension : {".dat", ".json"}) {
            auto& timeline = load(base + extension);
            if (auto it = timeline.clips.find(key); it != timeline.clips.end()) return {&timeline, &it->second};
        }
        auto& timeline = load(base + "-" + std::string(animation) + ".json");
        if (auto it = timeline.clips.find(key); it != timeline.clips.end()) return {&timeline, &it->second};
        return {nullptr, nullptr};
    }
};

AfpLibrary::AfpLibrary() : impl_(std::make_unique<Impl>()) {}
AfpLibrary::~AfpLibrary() = default;
AfpLibrary::AfpLibrary(AfpLibrary&&) noexcept = default;
AfpLibrary& AfpLibrary::operator=(AfpLibrary&&) noexcept = default;

void AfpLibrary::releaseStageData() {
    std::erase_if(impl_->timelines, [](const auto& entry) {
        return entry.first.starts_with("songs/") && entry.first.find("/background/") != std::string::npos;
    });
    std::erase_if(impl_->originals, [](const auto& entry) {
        return entry.first.starts_with("songs/") && entry.first.ends_with("/background");
    });
}

void AfpLibrary::queueData(std::span<const std::string_view> folders, std::string_view extra) {
    for (const auto folder : folders) {
        if (std::ranges::find(impl_->queued, folder) == impl_->queued.end()) impl_->queued.emplace_back(folder);
    }
    if (!extra.empty() && !impl_->timelines.contains(std::string(extra)) &&
        std::ranges::find(impl_->queued, extra) == impl_->queued.end()) impl_->queued.emplace_back(extra);
    dataReady();
}

bool AfpLibrary::dataReady() {
    auto& state = *impl_;
    if (state.parsing.valid()) {
        if (state.parsing.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        auto prepared = state.parsing.get();
        state.originals.merge(prepared->originals);
        state.timelines.merge(prepared->timelines);
    }
    std::erase_if(state.queued, [&](const auto& path) {
        if (path.ends_with(".dat") || path.ends_with(".json")) return state.timelines.contains(path);
        const auto original = state.originals.find(path);
        return original != state.originals.end() && (!path.starts_with("crew/") || !original->second.empty() ||
            (state.timelines.contains(path + "/timeline.dat") && state.timelines.contains(path + "/timeline.json")));
    });
    if (state.queued.empty()) return true;
    state.parsing = std::async(std::launch::async, [paths = std::exchange(state.queued, {})] {
        auto prepared = std::make_unique<Impl>();
        for (const auto& path : paths) {
            if (path.ends_with(".dat") || path.ends_with(".json")) prepared->load(path);
            else prepared->loadFolder(path);
        }
        return prepared;
    });
    return false;
}

double AfpLibrary::duration(std::string_view folder, std::string_view animation, std::string_view label) {
    if (const auto* original = impl_->original(folder, animation, label))
        return original->count * 1000 / original->movie.fps();
    auto [_, clip] = impl_->get(folder, animation, label);
    if (!clip || !clip->fps) return 0;
    size_t count = clip->count();
    for (const auto& layer : clip->layers) count = std::max(count, layer.count());
    return count * 1000 / clip->fps;
}

Vec2 AfpLibrary::size(std::string_view folder, std::string_view animation, std::string_view label) {
    if (const auto* original = impl_->original(folder, animation, label)) return original->movie.size();
    auto [_, clip] = impl_->get(folder, animation, label);
    return clip ? clip->viewport : Vec2{0, 0};
}

Render::AfpAnchor AfpLibrary::anchor(std::string_view folder, std::string_view animation, std::string_view label,
                                   std::string_view instance) {
    const auto* original = impl_->original(folder, animation, label);
    return original ? original->movie.anchor(instance) : Render::AfpAnchor{};
}

void AfpLibrary::prefetch(Render& render, std::string_view folder, std::string_view animation,
                          std::string_view label, AfpTextures textures, bool deferred, bool stage) {
    if (textures == AfpTextures::None) return;
    if (const auto* original = impl_->original(folder, animation, label)) {
        original->movie.queueTextures(render, stage, deferred && textures != AfpTextures::Timeline);
        return;
    }
    auto [timeline, clip] = impl_->get(folder, animation, label);
    if (!clip) return;
    if (textures == AfpTextures::Timeline) {
        for (const auto& path : timeline->paths) render.imageSize(path);
        return;
    }

    std::vector<bool> loaded(timeline->paths.size());
    const auto load = [&](size_t texture) {
        if (loaded[texture]) return;
        if (deferred) render.queueImage(timeline->paths[texture], stage);
        else render.imageSize(timeline->paths[texture]);
        loaded[texture] = true;
    };
    if (!timeline->binary.empty()) {
        for (const auto texture : clip->textures) load(texture);
        return;
    }
    const auto loadFrames = [&](const Frames& frames) {
        for (const auto& frame : frames.frames)
            for (const auto& command : frame) load(command.texture);
    };
    if (clip->layers.empty()) loadFrames(*clip);
    else for (const auto& layer : clip->layers) loadFrames(layer);
}

bool AfpLibrary::draw(Render& render, std::string_view folder, std::string_view animation, std::string_view label,
                      double ms, bool loop, Rect bounds, const Bindings& bindings,
                      const std::array<Render::Projected, 3>* plane, float opacity) {
    if (auto* original = impl_->original(folder, animation, label)) {
        original->draw(render, ms, loop, bounds, bindings, plane, opacity);
        return true;
    }
    auto [timeline, clip] = impl_->get(folder, animation, label);
    if (!clip || (!clip->count() && clip->layers.empty())) return false;
    const size_t elapsedFrame = std::floor(std::max(0.0, ms) * clip->fps / 1000);
    auto frameIndex = [elapsedFrame, loop](size_t count, size_t repeatFrom = 0) {
        if (elapsedFrame < count) return elapsedFrame;
        return loop ? repeatFrom + (elapsedFrame - repeatFrom) % (count - repeatFrom) : count - 1;
    };
    float sx = bounds.w / clip->viewport.x, sy = bounds.h / clip->viewport.y;
    const auto project = [plane](Render::Projected p) {
        if (!plane) return p;
        const auto [origin, right, bottom] = *plane;
        return Render::Projected{
            origin.x * p.w + (right.x - origin.x) * p.x + (bottom.x - origin.x) * p.y,
            origin.y * p.w + (right.y - origin.y) * p.x + (bottom.y - origin.y) * p.y,
            origin.w * p.w + (right.w - origin.w) * p.x + (bottom.w - origin.w) * p.y
        };
    };
    Mask planeMask;
    if (plane) {
        for (auto point : {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}}) {
            const auto p = project({point.x, point.y, 1});
            planeMask.push_back({p.x / p.w, p.y / p.w});
        }
    }
    auto draw = [&](const Command& command) {
        const std::string& name = timeline->textures.at(command.texture);
        auto binding = bindings.find(command.binding.empty() ? name : command.binding);
        if (binding == bindings.end()) binding = bindings.find(name);
        if (binding != bindings.end() && binding->second.path.empty()) return;
        const auto& texture = timeline->paths[command.texture];
        Vec2 original = render.imageSize(texture);
        std::string_view image = binding == bindings.end() ? texture : binding->second.path;
        if (original.x == 0) original = render.imageSize(image);
        if (original.x == 0) return;
        Rect uv{0,0,1,1};
        Rect geometry{0,0,original.x,original.y};
        if (command.sourceRect.w > 0) {
            const auto& r = command.sourceRect;
            uv = {r.x/original.x, r.y/original.y, r.w/original.x, r.h/original.y};
            geometry = {0,0,r.w,r.h};
        }
        if (command.geometry.w > 0) geometry = command.geometry;
        if (binding != bindings.end()) {
            if (binding->second.targetRect.w > 0) geometry = binding->second.targetRect;
            uv = {0,0,1,1};
            if (binding->second.sourceRect.w > 0) {
                const auto size = render.imageSize(image);
                const auto& r = binding->second.sourceRect;
                uv = {r.x/size.x,r.y/size.y,r.w/size.x,r.h/size.y};
            }
        }
        const auto& m = command.matrix;
        auto transform = [&](float x, float y) {
            float w = m[6] * x + m[7] * y + m[8];
            return project({sx * (m[0] * x + m[1] * y + m[2]) + bounds.x * w,
                            sy * (m[3] * x + m[4] * y + m[5]) + bounds.y * w, w});
        };
        const auto& g = geometry;
        std::array corners{transform(g.x,g.y),transform(g.x+g.w,g.y),transform(g.x+g.w,g.y+g.h),transform(g.x,g.y+g.h)};
        std::span<const Mask> masks = command.masks;
        std::vector<Mask> transformed;
        if (plane || (!masks.empty() && (sx != 1 || sy != 1 || bounds.x != 0 || bounds.y != 0))) {
            transformed = command.masks;
            for (auto& mask : transformed) {
                for (auto& point : mask) {
                    const auto p = project({bounds.x + point.x * sx, bounds.y + point.y * sy, 1});
                    point = {p.x / p.w, p.y / p.w};
                }
            }
            if (plane) transformed.push_back(planeMask);
            masks = transformed;
        }
        auto multiply = command.multiply;
        auto add = command.add;
        if (binding != bindings.end()) {
            const auto tint = binding->second.tint;
            const auto extra = binding->second.add;
            add = {add.r + multiply.r * extra.r, add.g + multiply.g * extra.g,
                   add.b + multiply.b * extra.b, add.a + multiply.a * extra.a};
            multiply = {multiply.r*tint.r,multiply.g*tint.g,multiply.b*tint.b,multiply.a*tint.a};
        }
        multiply.a *= opacity;
        add.a *= opacity;
        render.projected(image, corners, multiply, add, command.blend, masks, uv, command.nearest, command.hsl);
    };
    auto drawFrames = [&](const Frames& frames, size_t repeatFrom) {
        const size_t frame = frameIndex(frames.count(), repeatFrom);
        if (frames.offsets.empty()) {
            for (const auto& command : frames.frames[frame]) draw(command);
            return;
        }
        if (frames.decodedFrame != frame) {
            Reader reader{timeline->binary, frames.offsets[frame]};
            auto commands = reader.integer(2);
            frames.decoded.clear();
            frames.decoded.reserve(commands);
            for (uint32_t i = 0; i < commands; ++i) {
                auto command = binaryCommand(reader, timeline->colorShift);
                if (!timeline->regions.empty()) {
                    const auto& region = timeline->regions.at(command.texture);
                    command.sourceRect = region.rect;
                    command.geometry = region.geometry;
                    command.nearest = region.nearest;
                }
                frames.decoded.push_back(std::move(command));
            }
            frames.decodedFrame = frame;
        }
        for (const auto& command : frames.decoded) draw(command);
    };
    if (!plane) render.pushClip(bounds);
    if (!clip->layers.empty()) {
        for (const auto& layer : clip->layers) drawFrames(layer, layer.repeatFrom);
    } else drawFrames(*clip, 0);
    if (!plane) render.popClip();
    return true;
}
}
