#include "afp_movie.h"

#include <bit>
#include <deque>
#include <nlohmann/json.hpp>
#include <SDL3/SDL_log.h>
#include <stdexcept>
#include <unordered_set>
#include <variant>

namespace sdvx {
namespace {

// AP2 and BSI layout follow the original files and the format description in
// bemaniutils/bemani/format/afp. BSI restores the original little-endian fields.
struct Bytes {
    std::string_view data;
    size_t at = 0;

    std::string_view take(size_t size) {
        if (at > data.size() || size > data.size() - at) throw std::runtime_error("Truncated AFP record");
        const auto value = data.substr(at, size);
        at += size;
        return value;
    }
    unsigned read(size_t size, bool big = false) {
        const auto bytes = take(size);
        unsigned value = 0;
        for (size_t i = 0; i < size; ++i)
            value |= unsigned(uint8_t(bytes[big ? size - i - 1 : i])) << (8 * i);
        return value;
    }
    uint8_t u8() { return read(1); }
    uint16_t u16() { return read(2); }
    uint32_t u32() { return read(4); }
    int16_t i16() { return std::bit_cast<int16_t>(u16()); }
    int32_t i32() { return std::bit_cast<int32_t>(u32()); }
    void align() { take((4 - at % 4) % 4); }
    std::string_view string(size_t offset) const {
        auto tail = data.substr(offset);
        const auto end = tail.find('\0');
        if (end == tail.npos) throw std::runtime_error("Unterminated AFP string");
        return tail.substr(0, end);
    }
};

std::string restore(std::string data, std::string_view bsi) {
    Bytes script{bsi};
    size_t offset = 0;
    while (const auto op = script.u16()) {
        offset += (op & 127) * 2;
        const auto type = op >> 13;
        const size_t count = (op >> 7) & 63;
        if (!type) {
            offset += count * 256;
            continue;
        }
        if (type > 3) throw std::runtime_error("Unsupported BSI swap size");
        const size_t width = size_t{1} << type;
        Bytes check{data, offset};
        check.take(width * (count + 1));
        for (size_t i = 0; i <= count; ++i, offset += width)
            std::reverse(data.begin() + offset, data.begin() + offset + width);
    }
    Bytes header{data, 4};
    if (data.size() < 56 || (uint8_t(data[3]) & 127) != 'A' ||
        (uint8_t(data[2]) & 127) != 'P' || (uint8_t(data[1]) & 127) != '2' ||
        header.u32() != data.size() || header.u16() != 0x200)
        throw std::runtime_error("Unsupported AFP header");
    header.at = 48;
    const auto start = header.u32(), size = header.u32();
    Bytes{data, start}.take(size);
    for (size_t i = 0; i < size; ++i) data[start + i] = char(uint8_t(data[start + i] - 128 - i));
    return data;
}

using Matrix = std::array<double, 12>;
using Projection = std::array<double, 9>;
using Channels = std::array<double, 4>;
using Shift = std::array<double, 3>;
constexpr Matrix Identity{1,0,0, 0,1,0, 0,0,1, 0,0,0};
constexpr Channels Opaque{1,1,1,1};
using Mask = std::vector<Vec2>;
using ColorMatrix = Render::ColorMatrix;

ColorMatrix composeColor(const ColorMatrix& outer, const ColorMatrix& inner) {
    ColorMatrix result{};
    for (size_t row = 0; row < 4; ++row) {
        result[row * 5 + 4] = outer[row * 5 + 4];
        for (size_t column = 0; column < 5; ++column)
            for (size_t channel = 0; channel < 4; ++channel)
                result[row * 5 + column] += outer[row * 5 + channel] * inner[channel * 5 + column];
    }
    return result;
}

struct MaskVertex {
    Vec2 anchor;
    Vec2 incoming, outgoing;
};
struct VectorMask {
    unsigned slot;
    uint16_t options;
    Mask path;
};

struct Action {
    enum Kind { Go, GoStop, DeepGo, DeepStop, Play, Stop, PlayTree, StopTree, RectMask, FrameControl, FrameOffset } kind;
    std::string_view label;
    std::array<double, 4> values{};
    std::optional<uint16_t> depth{};
};
struct Placement {
    uint32_t flags;
    uint16_t depth, id;
    std::optional<uint16_t> source;
    std::string_view name;
    uint8_t projection = 0, blend = 0;
    Matrix matrix = Identity;
    uint16_t matrixFields = 0;
    std::array<double, 3> origin{};
    bool hasOrigin = false;
    bool hasTransform = false;
    Channels multiply = Opaque, add{};
    std::optional<Shift> shift;
    std::optional<ColorMatrix> filter;
    std::optional<uint16_t> bitmapImage;
    std::optional<Color> solidColor;
    Vec2 overlaySize{};
    std::vector<Action> onLoad;
    std::vector<VectorMask> vectorMasks;
};
struct Removal { uint16_t id, depth; };
struct Camera { double x, y, z, focal; };
using Actions = std::vector<Action>;
using Tag = std::variant<std::monostate, Placement, Removal, Camera, Actions>;
struct Frame { size_t first, end; };
struct Timeline {
    std::unordered_map<std::string_view, size_t> labels;
    std::vector<Frame> frames;
    std::vector<Tag> tags;
    struct Loop { size_t frame; bool descendants; bool rewind; };
    std::unordered_map<size_t, Loop> loops;
    bool staticHolder = false;
};
struct Movie;
struct Surface {
    std::string path;
    std::string key;
    Rect bounds{};
    Rect source{};
    bool nearest = false;
    bool runtime = false;
    Color color = White;
};
struct Definition {
    std::unique_ptr<Timeline> timeline;
    std::vector<Surface> surfaces;
    const Movie* owner = nullptr;
};

struct Movie {
    std::string raw, folder, name;
    Bytes strings;
    Timeline root;
    std::unordered_map<uint16_t, Definition> definitions;
    std::unordered_map<std::string_view, uint16_t> exports;
    std::unordered_map<uint16_t, const Definition*> imports;
    std::unordered_map<std::string_view, std::unique_ptr<Movie>> dependencies;
    nlohmann::json regions = nlohmann::json::object();
    std::vector<std::string> images;
    double fps;
    Vec2 size;

    Movie(std::string_view path, std::string_view animation, unsigned depth = 0) : folder(path), name(animation) {
        if (depth > 16) throw std::runtime_error("Recursive AFP imports in " + name);
        const auto textureIndex = folder + "/afp-textures.json";
        if (hasAsset(textureIndex)) regions = nlohmann::json::parse(readAsset(textureIndex));
        raw = restore(readAsset(folder + "/afp/" + name), readAsset(folder + "/afp/bsi/" + name));
        Bytes header{raw, 12};
        const auto flags = header.u32();
        const auto left = header.u16(), right = header.u16(), top = header.u16(), bottom = header.u16();
        size = {float(right - left), float(bottom - top)};
        const auto rate = header.u32();
        fps = flags & 2 ? rate / 1024.0 : std::bit_cast<float>(rate);
        if (!(fps > 0) || size.x <= 0 || size.y <= 0) throw std::runtime_error("Invalid AFP dimensions or frame rate");
        const auto rootOffset = Bytes{raw, 36}.u32();
        header.at = 48;
        const auto stringOffset = header.u32(), stringSize = header.u32();
        strings = {Bytes{raw, stringOffset}.take(stringSize)};
        header.at = 32;
        const auto exportCount = header.u16(), importCount = header.u16();
        header.at = 40;
        const auto exportsOffset = header.u32(), importsOffset = header.u32();
        Bytes exported{raw, exportsOffset};
        for (size_t i = 0; i < exportCount; ++i) {
            const auto id = exported.u16(), symbol = exported.u16();
            exports.emplace(strings.string(symbol), id);
        }
        root = timeline(std::string_view(raw).substr(rootOffset));
        Bytes importHeaders{raw, importsOffset}, importEntries{raw, importsOffset + size_t(importCount) * 4};
        for (size_t i = 0; i < importCount; ++i) {
            const auto library = strings.string(importHeaders.u16());
            const auto count = importHeaders.u16();
            for (size_t j = 0; j < count; ++j) {
                const auto id = importEntries.u16();
                const auto symbol = strings.string(importEntries.u16());
                if (library == "aeplib" || library == "asdlib" ||
                    library.starts_with("aeplib.") || library.starts_with("asdlib.")) {
                    // The original places script-package exports at depth zero.
                    // Their methods execute below and the package has no artwork.
                    definitions[id].owner = this;
                    continue;
                }
                if (library == name) {
                    imports.emplace(id, &definition(exports.at(symbol)));
                } else {
                    auto& dependency = dependencies[library];
                    if (!dependency) dependency = std::make_unique<Movie>(folder, library, depth + 1);
                    imports.emplace(id, &dependency->definition(dependency->exports.at(symbol)));
                }
            }
        }
        if (depth == 0) images = reachableImages(root);
    }

    std::vector<std::string> reachableImages(const Timeline& timeline) const {
        std::unordered_set<const Definition*> visited;
        std::vector<std::string> paths;
        const auto collect = [&](auto&& self, const Movie& owner, const Timeline& active) -> void {
            for (const auto& tag : active.tags) {
                const auto* placement = std::get_if<Placement>(&tag);
                if (!placement) continue;
                for (const auto id : {placement->source, placement->bitmapImage}) {
                    if (!id) continue;
                    const auto& source = owner.definition(*id);
                    if (!visited.insert(&source).second) continue;
                    for (const auto& surface : source.surfaces)
                        if (!surface.runtime && !surface.path.empty()) paths.push_back(surface.path);
                    if (source.timeline) self(self, *source.owner, *source.timeline);
                }
            }
        };
        collect(collect, *this, timeline);
        std::ranges::sort(paths);
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        for (const auto& image : paths) {
            if (!hasAsset(image)) throw std::runtime_error("Missing AFP texture " + image);
        }
        return paths;
    }

    Surface texture(std::string_view region, Rect bounds = {}) const {
        Surface result{.path = folder + "/tex/" + std::string(region) + ".png", .key = std::string(region), .bounds = bounds};
        if (const auto entry = regions.find(region); entry != regions.end()) {
            if (const auto crop = entry->find("source_rect"); crop != entry->end()) {
                const auto area = crop->get<std::array<float, 4>>();
                result.source = {area[0], area[1], area[2], area[3]};
            }
            result.nearest = entry->value("nearest", false);
            result.runtime = entry->value("runtime", false);
        }
        return result;
    }

    const Definition& definition(uint16_t id) const {
        if (const auto def = definitions.find(id); def != definitions.end()) return def->second;
        if (const auto def = imports.find(id); def != imports.end()) return *def->second;
        throw std::runtime_error("Missing AFP definition " + name + ":" + std::to_string(id));
    }

    Actions actions(std::string_view data) const {
        Bytes code{data};
        if (code.u8() != 0xff) throw std::runtime_error("Unsupported AFP action encoding");
        std::vector<std::string_view> constants;
        if (code.u8() & 1) {
            const auto count = code.u16();
            constants.reserve(count);
            for (size_t i = 0; i < count; ++i) constants.push_back(strings.string(code.u16()));
        }
        enum Receiver { This, Root, Parent, Library, SoundLibrary };
        struct Child { uint16_t depth; };
        using Value = std::variant<double, unsigned, std::string_view, Receiver, Child>;
        std::array<Value, 256> registers{};
        std::vector<Value> stack;
        Actions result;
        auto pop = [&]() {
            if (stack.empty()) throw std::runtime_error("Invalid AFP action stack");
            auto value = stack.back();
            stack.pop_back();
            return value;
        };
        auto number = [&]() { return std::get<double>(pop()); };
        auto readFrame = [&](Action& action) {
            const auto value = pop();
            if (const auto* label = std::get_if<std::string_view>(&value)) action.label = *label;
            else if (const auto* frame = std::get_if<double>(&value)) action.values[0] = *frame;
            else if (const auto* symbol = std::get_if<unsigned>(&value); symbol && *symbol == 0x20e) action.label = "normal";
            else if (symbol && *symbol == 0x45a) action.label = "clear";
            else throw std::runtime_error("Unsupported AFP frame argument");
        };
        while (code.at < code.data.size()) {
            switch (const auto opcode = code.u8()) {
            case 0: return result;
            case 3: result.push_back({Action::Play, {}}); break;
            case 4: result.push_back({Action::Stop, {}}); break;
            case 13: pop(); break;
            case 14: {
                const auto variable = std::get<unsigned>(pop());
                if (variable != 0x390 && variable != 0x320)
                    throw std::runtime_error("Unsupported AFP variable " + std::to_string(variable));
                stack.emplace_back(variable == 0x390 ? Library : SoundLibrary);
                break;
            }
            case 30: {
                const auto function = std::get<unsigned>(pop());
                if (function != 0x465 || number() != 1) throw std::runtime_error("Unsupported AFP function");
                stack.emplace_back(Child{uint16_t(int(number()) + 0x4000)});
                break;
            }
            case 47: {
                const auto frame = number();
                const auto member = std::get<std::string_view>(pop());
                const auto child = std::get<Child>(pop());
                if (member != "frameOffset") throw std::runtime_error("Unsupported AFP member");
                result.push_back({Action::FrameOffset, {}, {frame}, child.depth});
                break;
            }
            case 50: {
                const auto function = std::get<unsigned>(pop());
                const auto receiver = pop();
                const auto count = number();
                const auto child = std::get_if<Child>(&receiver);
                if (!child && std::get<Receiver>(receiver) == SoundLibrary && function == 0x800 && count == 1) {
                    // The stage plays the selected arrangement on the BASS song clock.
                    // The authored preview-music call is superseded by that transport.
                    pop();
                    stack.emplace_back(0.0);
                    break;
                }
                if (!child && (std::get<Receiver>(receiver) != Library || std::get<Receiver>(pop()) != This))
                    throw std::runtime_error("Unsupported AFP clip receiver");
                switch (function) {
                case 0x442: case 0x443: case 0x815: case 0x816: {
                    Action action{function == 0x442 ? Action::Go : function == 0x443 ? Action::GoStop :
                                  function == 0x815 ? Action::DeepGo : Action::DeepStop, {}};
                    readFrame(action);
                    if (child) action.depth = child->depth;
                    result.push_back(action);
                    break;
                }
                case 0x833:
                    result.push_back({Action::RectMask, {}, {number(), number(), number(), number()}});
                    break;
                case 0x832:
                    result.push_back({Action::FrameControl, {}, {number(), number()}});
                    break;
                case 0x440: result.push_back({Action::Stop, {}}); break;
                case 0x441: result.push_back({Action::Play, {}}); break;
                case 0x813: case 0x814: {
                    Action action{function == 0x813 ? Action::PlayTree : Action::StopTree, {}};
                    if (child) action.depth = child->depth;
                    result.push_back(action);
                    break;
                }
                case 0x836:
                    // The frame notification has no consumer in the stage scene.
                    pop();
                    break;
                default: throw std::runtime_error("Unsupported AFP method " + std::to_string(function));
                }
                if (count < 1) throw std::runtime_error("Missing AFP method arguments");
                stack.emplace_back(0.0);
                break;
            }
            case 63: {
                const auto count = code.u8();
                const auto value = pop();
                for (size_t i = 0; i < count; ++i) registers[code.u8()] = value;
                stack.push_back(value);
                break;
            }
            case 74: registers[code.u8()] = pop(); break;
            case 71: {
                const auto flags = code.u8();
                const auto offset = flags & 2 ? code.read(2, true) : 0;
                Action action{flags & 1 ? Action::GoStop : Action::Go, {}};
                readFrame(action);
                action.values[1] = offset;
                result.push_back(action);
                break;
            }
            case 67: {
                const auto count = code.u8();
                for (size_t i = 0; i < count; ++i) {
                    switch (const auto type = code.u8()) {
                    case 0: stack.emplace_back(0.0); break;
                    case 1: stack.emplace_back(double(std::bit_cast<float>(code.read(4, true)))); break;
                    case 4: stack.push_back(registers[code.u8()]); break;
                    case 7: stack.emplace_back(double(std::bit_cast<int32_t>(code.read(4, true)))); break;
                    case 8: stack.emplace_back(constants.at(code.u8())); break;
                    case 9: stack.emplace_back(constants.at(code.read(2, true))); break;
                    case 12: case 15: stack.emplace_back(This); break;
                    case 13: stack.emplace_back(Root); break;
                    case 14: stack.emplace_back(Parent); break;
                    case 19: stack.emplace_back(0x300u + code.u8()); break;
                    case 22: stack.emplace_back(0x400u + code.u8()); break;
                    case 25: stack.emplace_back(0x200u + code.u8()); break;
                    case 39: stack.emplace_back(0x800u + code.u8()); break;
                    case 55: stack.emplace_back(double(std::bit_cast<int8_t>(code.u8()))); break;
                    default: throw std::runtime_error("Unsupported AFP action value " + std::to_string(type));
                    }
                }
                break;
            }
            default: throw std::runtime_error("Unsupported AFP opcode " + std::to_string(opcode));
            }
        }
        return result;
    }

    Placement placement(Bytes tag) const {
        Placement p{};
        const auto flags = p.flags = tag.u32();
        p.depth = tag.u16();
        p.id = tag.u16();
        const uint32_t extended = flags & 0x80000000 ? tag.u32() : 0;
        if (flags & 2) p.source = tag.u16();
        if (flags & 0x10) tag.u16();
        if (flags & 0x20) p.name = strings.string(tag.u16());
        if (flags & 0x40) tag.u16();
        if (flags & 0x20000) p.blend = tag.u8();
        tag.align();
        auto matrixPair = [&](int x, int y, auto first, auto second, double divisor) {
            p.matrix[x] = first / divisor;
            p.matrix[y] = second / divisor;
            p.matrixFields |= (1 << x) | (1 << y);
        };
        if (flags & 0x100) {
            const auto a = tag.i32(), d = tag.i32();
            matrixPair(0, 4, a, d, 1024);
        }
        if (flags & 0x200) {
            const auto b = tag.i32(), c = tag.i32();
            matrixPair(1, 3, b, c, 1024);
        }
        if (flags & 0x400) {
            const auto x = tag.i32(), y = tag.i32();
            matrixPair(9, 10, x, y, 20);
        }
        if (flags & 0x800) for (auto& channel : p.multiply) channel = tag.i16() / 255.0;
        if (flags & 0x1000) for (auto& channel : p.add) channel = tag.i16() / 255.0;
        auto packed = [](uint32_t rgba) {
            return Channels{(rgba >> 24) / 255.0, ((rgba >> 16) & 255) / 255.0,
                            ((rgba >> 8) & 255) / 255.0, (rgba & 255) / 255.0};
        };
        if (flags & 0x2000) p.multiply = packed(tag.u32());
        if (flags & 0x4000) p.add = packed(tag.u32());
        if (flags & 0x80) {
            const auto eventFlags = tag.u32(), length = tag.u32();
            if (length < 8) throw std::runtime_error("Invalid AFP clip event length");
            const auto start = tag.at - 8;
            auto events = Bytes{tag.data.substr(start, length), 10};
            if (eventFlags) {
                const auto count = events.u16();
                for (size_t i = 0; i < count; ++i) {
                    const auto entry = events.at;
                    const auto trigger = events.u32();
                    events.u16();
                    const auto offset = entry + events.u16();
                    if (trigger != 1) throw std::runtime_error("Unsupported AFP clip event");
                    auto parsed = actions(events.data.substr(offset));
                    p.onLoad.insert(p.onLoad.end(), parsed.begin(), parsed.end());
                }
            }
            tag.take(length - 8);
        }
        if (flags & 0x10000) {
            const auto base = tag.at;
            const auto count = tag.u16();
            const auto length = tag.u16();
            const auto header = (size_t{4} + count * 2 + 3) & ~size_t{3};
            if (length < header) throw std::runtime_error("Invalid AFP filter length");
            Bytes block{tag.data.substr(base, length), 4};
            tag.take(length - 4);
            for (size_t i = 0; i < count; ++i) {
                const size_t start = block.u16();
                const size_t end = i + 1 < count ? Bytes{block.data, block.at}.u16() : length;
                if (start < header || end <= start || end > length)
                    throw std::runtime_error("Invalid AFP filter offsets");
                Bytes filter{block.data.substr(start, end - start)};
                if (const auto type = filter.u8(); type == 6 && (end - start == 84 || end - start == 88)) {
                    // The optional HSV fields describe the edit used to generate the stored matrix.
                    filter.take(3);
                    ColorMatrix matrix;
                    for (auto& value : matrix) value = filter.i32() / 65536.f;
                    p.filter = p.filter ? composeColor(matrix, *p.filter) : matrix;
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "AFP %s filter 0x%02x", name.c_str(), unsigned(type));
                }
            }
        }
        if (flags & 0x1000000) {
            p.origin[0] = tag.i32() / 20.0;
            p.origin[1] = tag.i32() / 20.0;
            p.hasOrigin = true;
        }
        if (extended & 2) {
            p.origin[2] = tag.i32() / 20.0;
            p.hasOrigin = true;
        }
        if (flags & 0x2000000) {
            p.origin = {};
            p.hasOrigin = true;
        }
        if (flags & 0x40000) {
            const auto a = tag.i16(), d = tag.i16();
            matrixPair(0, 4, a, d, 32768);
        }
        if (flags & 0x80000) {
            const auto b = tag.i16(), c = tag.i16();
            matrixPair(1, 3, b, c, 32768);
        }
        if (flags & 0x100000) tag.u16();
        tag.align();
        if (flags & 0x8000000) {
            p.matrix[11] = tag.i32() / 20.0;
            p.matrixFields |= 1 << 11;
        }
        if (flags & 0x10000000) {
            for (size_t i = 0; i < 9; ++i) {
                const auto value = tag.i32() / 1024.0;
                if (!(p.matrixFields & (1 << i))) p.matrix[i] = value;
            }
            p.matrixFields |= 0x1ff;
        }
        if (flags & 0x20000000)
            p.shift = Shift{tag.i16() / 360.0, std::bit_cast<int8_t>(tag.u8()) / 100.0,
                            std::bit_cast<int8_t>(tag.u8()) / 100.0};
        if (extended & 8) {
            auto slots = tag.u32();
            for (; slots; slots &= slots - 1) {
                const auto options = tag.u16(), count = tag.u16();
                const auto coordinate = [&] { return float((options & 1 ? tag.i32() : tag.i16()) / 20.); };
                const auto point = [&] { return Vec2{coordinate(), coordinate()}; };
                auto& path = p.vectorMasks.emplace_back(VectorMask{unsigned(std::countr_zero(slots)), options, {}}).path;
                if (!(options & 16)) {
                    path.reserve(count);
                    for (size_t i = 0; i < count; ++i) path.push_back(point());
                    continue;
                }
                std::vector<MaskVertex> vertices;
                vertices.reserve(count);
                for (size_t i = 0; i < count; ++i) vertices.push_back({point(), point(), point()});
                for (size_t i = 0; i < vertices.size(); ++i) {
                    const auto& a = vertices[i];
                    const auto& b = vertices[(i + 1) % vertices.size()];
                    path.push_back(a.anchor);
                    if (a.outgoing.x == 0 && a.outgoing.y == 0 && b.incoming.x == 0 && b.incoming.y == 0)
                        continue;
                    for (int sample = 1; sample < 12; ++sample) {
                        const float t = sample / 12.f, s = 1 - t;
                        const auto cubic = [&](float first, float out, float last, float in) {
                            return s*s*s*first + 3*s*s*t*(first+out) + 3*s*t*t*(last+in) + t*t*t*last;
                        };
                        path.push_back({cubic(a.anchor.x,a.outgoing.x,b.anchor.x,b.incoming.x),
                                        cubic(a.anchor.y,a.outgoing.y,b.anchor.y,b.incoming.y)});
                    }
                }
            }
        }
        if (extended & 16) {
            p.solidColor = Color{tag.u8() / 255.f, tag.u8() / 255.f, tag.u8() / 255.f, tag.u8() / 255.f};
            p.overlaySize = {float(tag.i16()), float(tag.i16())};
        }
        if (extended & 32) {
            p.bitmapImage = tag.u16();
            p.overlaySize = {float(tag.i16()), float(tag.i16())};
        }
        tag.align();
        constexpr uint32_t placementFlags = 0x80000000 | 0x3f1f0000 | 0x7fff;
        if ((flags & ~placementFlags) || (extended & ~0x3bu) || tag.at != tag.data.size())
            throw std::runtime_error("Unsupported AFP placement flags " + std::to_string(flags));
        p.hasTransform = (flags & 0x18000004) != 0;
        p.projection = flags & 0x4000000 ? 2 : p.hasTransform ? 1 : 0;
        if (p.projection != 2) {
            p.origin[2] = 0;
            for (auto i : {2, 5, 6, 7, 8, 11}) p.matrix[i] = Identity[i];
            p.matrixFields &= 0x61b;
        }
        return p;
    }

    Timeline timeline(std::string_view data) {
        Bytes header{data, 2};
        const auto labelCount = header.u16();
        const auto frameCount = header.u32(), tagCount = header.u32();
        const auto labelsOffset = header.u32(), framesOffset = header.u32(), tagsOffset = header.u32();
        Timeline result;
        Bytes labels{Bytes{data, labelsOffset}.take(size_t(labelCount) * 4)};
        for (size_t i = 0; i < labelCount; ++i) {
            const auto frame = labels.u16(), nameOffset = labels.u16();
            if (frame > frameCount) throw std::runtime_error("AFP label outside timeline");
            result.labels.emplace(strings.string(nameOffset), frame);
        }
        Bytes frames{Bytes{data, framesOffset}.take(size_t(frameCount) * 4)};
        result.frames.reserve(frameCount);
        for (size_t i = 0; i < frameCount; ++i) {
            const auto packed = frames.u32();
            const auto first = packed & 0xfffff, end = first + (packed >> 20);
            if (end > tagCount) throw std::runtime_error("AFP frame outside tag table");
            result.frames.push_back({first, end});
        }
        Bytes tags{data, tagsOffset};
        result.tags.reserve(tagCount);
        for (size_t i = 0; i < tagCount; ++i) {
            const auto packed = tags.u32();
            const auto body = tags.take(packed & 0x3fffff);
            tags.align();
            Bytes fields{body};
            switch (const auto type = packed >> 22) {
            case 0x79: {
                const auto flags = fields.u16(), id = fields.u16();
                const auto offset = flags & 1 ? fields.u32() : 4;
                auto child = std::make_unique<Timeline>(timeline(body.substr(offset)));
                definitions[id].timeline = std::move(child);
                definitions[id].owner = this;
                result.tags.emplace_back();
                break;
            }
            case 0x7a: result.tags.emplace_back(actions(body)); break;
            case 0x7f: result.tags.emplace_back(placement(fields)); break;
            case 0x80: result.tags.emplace_back(Removal{fields.u16(), fields.u16()}); break;
            case 0x83: {
                const auto flags = fields.u32();
                const auto id = fields.u16(), offset = fields.u16();
                const auto region = (flags & 2 ? "SWFA-" : "") + std::string(strings.string(offset));
                definitions[id].surfaces.push_back(texture(region));
                definitions[id].owner = this;
                result.tags.emplace_back();
                break;
            }
            case 0x84: {
                fields.u16();
                const auto id = fields.u16();
                definitions[id].surfaces = geometry(name + "_shape" + std::to_string(id));
                definitions[id].owner = this;
                result.tags.emplace_back();
                break;
            }
            case 0x88: {
                const auto flags = fields.u16();
                fields.u16();
                Camera camera{};
                if (flags & 1) {
                    camera.x = fields.i32() / 20.0;
                    camera.y = fields.i32() / 20.0;
                    camera.z = fields.i32() / 20.0;
                }
                if (flags & 2) camera.focal = fields.i32() / 20.0;
                result.tags.emplace_back(camera);
                break;
            }
            default: throw std::runtime_error("Unsupported AFP tag " + std::to_string(type));
            }
        }
        result.staticHolder = result.frames.size() > 1 &&
            std::all_of(result.frames.begin() + 1, result.frames.end(), [](Frame f) { return f.first == f.end; });
        for (size_t i = 0; i < result.frames.size(); ++i) {
            const auto [first, end] = result.frames[i];
            for (size_t tag = first; tag < end; ++tag) {
                const auto* script = std::get_if<Actions>(&result.tags[tag]);
                if (!script || script->size() != 1) continue;
                const auto& action = script->front();
                if ((action.kind != Action::Go && action.kind != Action::DeepGo) ||
                    action.label.empty() || action.depth) continue;
                const auto target = result.labels.find(action.label);
                if (target != result.labels.end() && target->second < i)
                    result.loops.emplace(i, Timeline::Loop{target->second, action.kind == Action::DeepGo,
                        target->second == 0 && action.label == "non"});
            }
        }
        return result;
    }

    std::vector<Surface> geometry(const std::string& shape) {
        const auto data = readAsset(folder + "/geo/" + shape);
        const bool big = data.starts_with("GE2D");
        if (!big && !data.starts_with("D2EG")) throw std::runtime_error("Missing AFP geometry " + shape);
        Bytes header{data, 28};
        const auto count = header.read(2, big);
        header.at = 44;
        const auto names = header.read(4, big), draws = header.read(4, big);
        Rect bounds{};
        {
            const auto count = Bytes{data, 20}.read(2, big);
            Bytes points{data, Bytes{data, 32}.read(4, big)};
            float left = INFINITY, right = -INFINITY, top = INFINITY, bottom = -INFINITY;
            for (size_t i = 0; i < count; ++i) {
                const auto x = std::bit_cast<float>(points.read(4, big));
                const auto y = std::bit_cast<float>(points.read(4, big));
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
            if (count) bounds = {left, top, right - left, bottom - top};
        }
        std::vector<Surface> surfaces;
        for (size_t i = 0; i < count; ++i) {
            Bytes draw{data, draws + i * 16};
            const auto mode = draw.u8(), flags = draw.u8(), texture = draw.u8();
            if (!(flags & 1)) break;
            if (mode != 4) throw std::runtime_error("Unsupported AFP geometry mode");
            if (flags & 2) {
                const auto offset = Bytes{data, names + size_t(texture) * 4}.read(4, big);
                std::string region(Bytes{data}.string(offset));
                if (!region.empty() && uint8_t(region.front()) >= 128)
                    for (auto& byte : region) byte = char(uint8_t(byte) ^ 128);
                surfaces.push_back(this->texture(region, bounds));
            } else if (flags & 8) {
                const auto rgba = Bytes{data, draws + i * 16 + 8}.read(4, big);
                Surface solid;
                solid.key = "shape_" + shape + ", untextured";
                solid.bounds = bounds;
                solid.color = {(rgba >> 24) / 255.f, ((rgba >> 16) & 255) / 255.f,
                               ((rgba >> 8) & 255) / 255.f, (rgba & 255) / 255.f};
                surfaces.push_back(std::move(solid));
            }
        }
        return surfaces;
    }
};

struct Instance {
    const Definition* source = nullptr;
    const Timeline* timeline = nullptr;
    std::string_view name;
    std::string instancePath;
    uint16_t id = 0, depth = 0;
    Matrix matrix = Identity;
    std::array<double, 3> origin{};
    Channels multiply = Opaque, add{};
    Shift shift{};
    const ColorMatrix* filter = nullptr;
    const Surface* bitmap = nullptr;
    Vec2 bitmapSize{};
    std::optional<Color> solid;
    Vec2 solidSize{};
    uint8_t projection = 0, blend = 0;
    std::optional<std::array<double, 4>> mask;
    std::optional<Camera> camera;
    std::vector<const VectorMask*> vectorMasks;
    std::vector<std::unique_ptr<Instance>> children;
    size_t frame = 0, tagCursor = 0, visibleFrom = 0;
    bool playing = true;
    bool skipActionsBeforeLabel = false;
    bool visible = true;
    std::optional<size_t> requested;

    void rewind() {
        frame = tagCursor = 0;
        children.clear();
        camera.reset();
    }
};

void setPlaying(Instance& instance, bool playing) {
    instance.playing = playing;
    for (auto& child : instance.children) setPlaying(*child, playing);
}

Matrix compose(const Matrix& local, const Matrix& parent, const std::array<double, 3>& origin) {
    Matrix result{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            const size_t i = row * 3 + column;
            result[i] = local[row * 3] * parent[column] + local[row * 3 + 1] * parent[3 + column] +
                        local[row * 3 + 2] * parent[6 + column];
            if (row == 3) result[i] += parent[i];
        }
    }
    for (size_t axis = 0; axis < 3; ++axis)
        result[9 + axis] = result[axis] * -origin[0] + result[3 + axis] * -origin[1] +
                           result[6 + axis] * -origin[2] + result[9 + axis];
    return result;
}

Color color(const Channels& channels) {
    return {float(channels[0]), float(channels[1]), float(channels[2]), float(channels[3])};
}

}

struct AfpMovie::Impl {
    struct Parameter {
        std::string path, fromLabel, toLabel;
        size_t first, last;
        double from, to;
    };
    std::shared_ptr<const Movie> parsedMovie;
    const Movie& movie;
    Instance root;
    double startTime = 0, rate = 1;
    size_t tick = 0;
    bool started = false;
    std::vector<Parameter> parameters;
    std::deque<std::vector<Instance*>> stepChildren;
    std::unordered_set<std::string> include, exclude, textureSlots, hiddenInstances, dimmedInstances;
    std::string rootInstance;
    double brightness = 1;
    std::unordered_map<std::string, std::string> textureAliases;
    std::unordered_map<std::string, std::string> texturePaths;
    std::unordered_map<std::string, std::string> textureInstances;
    struct Replacement {
        std::shared_ptr<const Movie> movie;
        const Definition* source;
        std::vector<std::string> textures;
    };
    std::unordered_map<std::string, Replacement> replacements;
    std::unordered_map<std::string, std::vector<std::string>> slotTextures;

    explicit Impl(std::shared_ptr<const Movie> parsed) : parsedMovie(std::move(parsed)), movie(*parsedMovie) {
        root.timeline = &movie.root;
    }

    void execute(Instance& instance, const Actions& actions, std::optional<size_t>& jump) {
        for (const auto& action : actions) {
            auto* clip = &instance;
            if (action.depth) {
                const auto child = std::ranges::find(instance.children, *action.depth,
                    [](const auto& item) { return item->depth; });
                if (child == instance.children.end() || !(*child)->timeline) continue;
                clip = child->get();
            }
            switch (action.kind) {
            case Action::Go: case Action::GoStop: case Action::DeepGo: case Action::DeepStop: {
                const auto& labels = clip->timeline->labels;
                auto label = labels.find(action.label);
                // City registers normal_loop, but its return action spells it nomal_loop.
                if (label == labels.end() && action.label == "nomal_loop") label = labels.find("normal_loop");
                if (!action.label.empty() && label == labels.end()) break;
                (clip == &instance ? jump : clip->requested) = size_t(action.values[1]) +
                    (action.label.empty() ? size_t(std::max(1.0, action.values[0])) :
                    std::max(size_t{1}, label->second));
                clip->playing = action.kind == Action::Go || action.kind == Action::DeepGo;
                if (action.kind == Action::DeepGo || action.kind == Action::DeepStop)
                    setPlaying(*clip, clip->playing);
                break;
            }
            case Action::FrameOffset: clip->requested = size_t(std::max(0.0, action.values[0])) + 1; break;
            case Action::Play: instance.playing = true; break;
            case Action::Stop: instance.playing = false; break;
            case Action::PlayTree: case Action::StopTree: setPlaying(*clip, action.kind == Action::PlayTree); break;
            case Action::RectMask: instance.mask = action.values; break;
            case Action::FrameControl: {
                for (auto& child : instance.children) if (child->depth == action.values[0] && child->timeline) {
                    const auto frame = child->requested.value_or(child->frame) + size_t(std::max(1.0, action.values[1])) - 1;
                    child->visibleFrom = std::clamp(frame, size_t{1}, child->timeline->frames.size());
                    child->visible = child->frame >= child->visibleFrom;
                    break;
                }
                break;
            }
            }
        }
    }

    void place(Instance& instance, const Placement& p, std::vector<Instance*>& existing, size_t depth) {
        auto& children = instance.children;
        auto found = std::find_if(children.rbegin(), children.rend(), [&](const auto& child) {
            return child->depth == p.depth && child->id == p.id;
        });
        bool update = p.flags & 1;
        const auto& owner = instance.source ? *instance.source->owner : movie;
        const auto* source = p.source ? &owner.definition(*p.source) : nullptr;
        std::string instancePath;
        if (!replacements.empty()) {
            instancePath = instance.instancePath;
            const auto name = !p.name.empty() ? p.name : found != children.rend() ? (*found)->name : std::string_view{};
            if (!name.empty()) {
                instancePath += '/';
                instancePath += name;
                if (const auto replace = replacements.find(instancePath); replace != replacements.end() && source)
                    source = replace->second.source;
            }
        }
        if (!update && found != children.rend() && (*found)->source == source) update = true;
        if (update && found == children.rend()) return;
        Instance* child;
        bool newClip = false;
        if (update) {
            child = found->get();
            if (source && child->source != source) {
                std::erase(existing, child);
                child->rewind();
                child->requested.reset();
                child->playing = true;
                child->source = source;
                child->timeline = source->timeline.get();
                newClip = child->timeline != nullptr;
            }
        } else {
            if (!source) throw std::runtime_error("AFP placement without a definition");
            auto object = std::make_unique<Instance>();
            child = object.get();
            child->source = source;
            child->timeline = source->timeline.get();
            child->id = p.id;
            child->depth = p.depth;
            children.insert(std::upper_bound(children.begin(), children.end(), p.depth,
                [](uint16_t depth, const auto& object) { return depth < object->depth; }), std::move(object));
            newClip = child->timeline != nullptr;
        }
        if (!p.name.empty()) child->name = p.name;
        child->instancePath = std::move(instancePath);
        if (p.hasTransform) {
            const auto fields = p.matrixFields ? p.matrixFields : p.projection == 2 ? 0xfff : 0x61b;
            for (size_t i = 0; i < p.matrix.size(); ++i)
                if (fields & (1 << i)) child->matrix[i] = p.matrix[i];
        }
        if (p.projection) child->projection = p.projection;
        if (p.hasOrigin) child->origin = p.origin;
        if (p.flags & 8) {
            child->multiply = p.multiply;
            child->add = p.add;
        }
        if (p.shift) child->shift = *p.shift;
        if (p.flags & 0x10000) child->filter = p.filter ? &*p.filter : nullptr;
        if (!child->solid && !child->bitmap) {
            if (p.solidColor) {
                child->solid = p.solidColor;
                child->solidSize = p.overlaySize;
            } else if (p.bitmapImage) {
                const auto& surfaces = owner.definition(*p.bitmapImage).surfaces;
                child->bitmap = surfaces.empty() ? nullptr : &surfaces.front();
                child->bitmapSize = p.overlaySize;
            }
        }
        if (p.blend) child->blend = p.blend;
        for (const auto& vectorMask : p.vectorMasks) {
            if (child->vectorMasks.size() <= vectorMask.slot) child->vectorMasks.resize(vectorMask.slot + 1);
            child->vectorMasks[vectorMask.slot] = vectorMask.path.empty() ? nullptr : &vectorMask;
        }
        if (newClip) {
            execute(*child, p.onLoad, child->requested);
            step(*child, false, depth + 1);
        }
    }

    void step(Instance& instance, bool onlyDirty, size_t depth = 0) {
        const auto& frames = instance.timeline->frames;
        if (frames.empty()) return;
        auto requested = std::exchange(instance.requested, {});
        size_t target = instance.frame;
        // Keep the traversal snapshot while placements add or remove children.
        // The deque keeps parent scratch storage stable during recursive steps.
        if (depth == stepChildren.size()) stepChildren.emplace_back();
        auto& existing = stepChildren[depth];
        existing.clear();
        const auto remove = [&](const Removal& removal) {
            auto& children = instance.children;
            const auto it = std::find_if(children.rbegin(), children.rend(), [&](const auto& child) {
                return child->depth == removal.depth && (!removal.id || child->id == removal.id);
            });
            if (it != children.rend()) {
                std::erase(existing, it->get());
                children.erase(std::next(it).base());
            }
        };
        if (requested) {
            target = std::min(*requested, frames.size());
        } else if (!onlyDirty && instance.playing && (!instance.frame || !instance.timeline->staticHolder)) {
            if (const auto loop = instance.timeline->loops.find(instance.frame); loop != instance.timeline->loops.end()) {
                const auto [first, end] = frames[instance.frame];
                if (loop->second.rewind) {
                    instance.rewind();
                } else {
                    for (size_t i = first; i < end; ++i) {
                        const auto& tag = instance.timeline->tags[i];
                        if (const auto* p = std::get_if<Placement>(&tag); p && (p->flags & 1))
                            place(instance, *p, existing, depth);
                        else if (const auto* removal = std::get_if<Removal>(&tag)) remove(*removal);
                    }
                }
                instance.frame = loop->second.frame;
                if (loop->second.descendants) setPlaying(instance, true);
            }
            if (instance.frame == frames.size() && &instance != &root) instance.rewind();
            target = std::min(instance.frame + 1, frames.size());
        }
        for (;;) {
            if (instance.frame > target) instance.rewind();
            existing.clear();
            existing.reserve(instance.children.size());
            for (const auto& child : instance.children) if (child->timeline) existing.push_back(child.get());
            std::optional<size_t> jump;
            if (instance.frame != target) {
                const auto [first, end] = frames[instance.frame];
                auto apply = [&](const Tag& tag, bool runActions) {
                    if (const auto* placement = std::get_if<Placement>(&tag)) place(instance, *placement, existing, depth);
                    else if (const auto* removal = std::get_if<Removal>(&tag)) remove(*removal);
                    else if (const auto* view = std::get_if<Camera>(&tag)) {
                        instance.camera = *view;
                    } else if (const auto* actions = std::get_if<Actions>(&tag); actions && runActions) {
                        execute(instance, *actions, jump);
                    }
                };
                if (first > instance.tagCursor) {
                    for (size_t i = instance.tagCursor; i < first; ++i)
                        apply(instance.timeline->tags[i], !instance.skipActionsBeforeLabel);
                }
                for (size_t i = first; i < end; ++i)
                    apply(instance.timeline->tags[i], !instance.skipActionsBeforeLabel || instance.frame + 1 == target);
                instance.tagCursor = std::max(instance.tagCursor, end);
                ++instance.frame;
                if (!instance.visible && instance.frame >= instance.visibleFrom) instance.visible = true;
            }
            for (auto* child : existing) step(*child, onlyDirty, depth + 1);
            if (jump) target = std::min(*jump, frames.size());
            if (instance.frame == target) break;
        }
        instance.skipActionsBeforeLabel = false;
    }

    bool dirty(const Instance& instance) const {
        if (instance.requested) return true;
        return std::ranges::any_of(instance.children, [&](const auto& child) {
            return child->timeline && dirty(*child);
        });
    }

    Instance* find(std::string_view path) {
        auto* instance = &root;
        while (!path.empty() && path != "/") {
            const auto slash = path.find('/');
            const auto name = path.substr(0, slash);
            const auto child = std::ranges::find(instance->children, name, [](const auto& item) { return item->name; });
            if (child == instance->children.end() || !(*child)->timeline) return nullptr;
            instance = child->get();
            path = slash == path.npos ? std::string_view{} : path.substr(slash + 1);
        }
        return instance;
    }

    size_t cueTick(double time) const {
        return size_t(std::max(0.0, std::ceil((time - startTime) * rate * movie.fps / 1000 - 1e-5)));
    }

    void advance(size_t target) {
        while (tick < target) {
            for (const auto& parameter : parameters) {
                if (tick < parameter.first || tick > parameter.last) continue;
                auto* instance = find(parameter.path);
                if (!instance) continue;
                const auto& labels = instance->timeline->labels;
                const auto begin = double(labels.at(parameter.fromLabel)), end = double(labels.at(parameter.toLabel));
                const double fraction = parameter.last == parameter.first ? 1. :
                    double(tick - parameter.first) / double(parameter.last - parameter.first);
                const auto frame = size_t(std::clamp(begin + std::trunc((end - begin) *
                    (parameter.from + (parameter.to - parameter.from) * fraction)),
                    0., double(instance->timeline->frames.size() - 1)));
                instance->frame = frame;
                instance->requested = frame + 1;
            }
            step(root, false);
            while (dirty(root)) step(root, true);
            ++tick;
        }
        std::erase_if(parameters, [&](const auto& parameter) { return parameter.last < tick; });
    }

    void draw(Render& render, const Instance& instance, const Matrix& parent, bool perspective, Channels multiply,
              Channels add, Shift shift, uint8_t blend, std::vector<Mask>& masks, Rect bounds,
              const Bindings& bindings, std::string_view bindingName,
              const std::array<Render::Projected, 3>* plane, float opacity, const ColorMatrix* filter,
              std::string_view parentPath, std::string_view pathBinding,
              const Camera* camera = nullptr, const Projection* outerProjection = nullptr) const {
        if (!instance.visible) return;
        if (!instance.name.empty() && (!hiddenInstances.empty() || !bindings.empty())) {
            const std::string name(instance.name);
            if (hiddenInstances.contains(name)) return;
            if (!bindings.empty() && (textureSlots.contains(name) || slotTextures.contains(name) || bindings.contains(name)))
                bindingName = instance.name;
        }
        std::string path;
        if ((!texturePaths.empty() || !textureInstances.empty()) && !instance.name.empty()) {
            path = parentPath;
            if (!path.empty()) path += '/';
            path += instance.name;
            parentPath = path;
            if (const auto configured = texturePaths.find(path); configured != texturePaths.end())
                pathBinding = configured->second;
        }
        const auto matrix = compose(instance.matrix, parent, instance.origin);
        perspective |= instance.projection == 2;
        for (size_t i = 0; i < 4; ++i) {
            add[i] += instance.add[i] * multiply[i];
            multiply[i] *= instance.multiply[i];
        }
        if (!dimmedInstances.empty() && dimmedInstances.contains(std::string(instance.name))) {
            for (size_t channel = 0; channel < 3; ++channel) multiply[channel] *= .5;
        }
        for (size_t i = 0; i < 3; ++i) shift[i] += instance.shift[i];
        if (instance.blend > 2) blend = instance.blend;
        if (multiply[3] == 0 && add[3] == 0) return;
        ColorMatrix combined;
        if (instance.filter) {
            if (filter) {
                combined = composeColor(*filter, *instance.filter);
                filter = &combined;
            } else filter = instance.filter;
        }
        auto projection = std::array{matrix[0], matrix[3], matrix[9],
                                     matrix[1], matrix[4], matrix[10], 0., 0., 1.};
        if (perspective && camera) {
            const auto [cx, cy, cz, focal] = *camera;
            const double depth = matrix[11] - cz;
            projection = {focal * matrix[0] + cx * matrix[2], focal * matrix[3] + cx * matrix[5],
                          focal * (matrix[9] - cx) + cx * depth,
                          focal * matrix[1] + cy * matrix[2], focal * matrix[4] + cy * matrix[5],
                          focal * (matrix[10] - cy) + cy * depth, matrix[2], matrix[5], depth};
        }
        if (outerProjection) {
            Projection combined{};
            for (size_t row = 0; row < 3; ++row)
                for (size_t column = 0; column < 3; ++column)
                    for (size_t axis = 0; axis < 3; ++axis)
                        combined[row * 3 + column] += (*outerProjection)[row * 3 + axis] * projection[axis * 3 + column];
            projection = combined;
        }
        const float sx = bounds.w / movie.size.x, sy = bounds.h / movie.size.y;
        const auto onPlane = [plane](Render::Projected p) {
            if (!plane) return p;
            const auto& [o, r, b] = *plane;
            return Render::Projected{
                o.x * p.w + (r.x - o.x) * p.x + (b.x - o.x) * p.y,
                o.y * p.w + (r.y - o.y) * p.x + (b.y - o.y) * p.y,
                o.w * p.w + (r.w - o.w) * p.x + (b.w - o.w) * p.y};
        };
        const auto project = [&](double x, double y) {
            return std::array{projection[0] * x + projection[1] * y + projection[2],
                              projection[3] * x + projection[4] * y + projection[5],
                              projection[6] * x + projection[7] * y + projection[8]};
        };
        const auto maskPoint = [&](double x, double y) {
            const auto [px, py, w] = project(x, y);
            const auto p = onPlane({bounds.x + float(px / w) * sx, bounds.y + float(py / w) * sy, 1});
            return Vec2{p.x / p.w, p.y / p.w};
        };
        const auto inheritedMasks = masks.size();
        if (instance.mask) {
            const auto [left, right, top, bottom] = *instance.mask;
            masks.push_back({maskPoint(left, top), maskPoint(right, top), maskPoint(right, bottom), maskPoint(left, bottom)});
        }
        const Mask* fillContour = nullptr;
        bool paintFill = instance.solid.has_value() || instance.bitmap;
        for (const auto* vectorMask : instance.vectorMasks) {
            if (!vectorMask) continue;
            const auto& contour = vectorMask->path;
            if (paintFill) {
                paintFill = !fillContour && !(vectorMask->options & 0x40);
                fillContour = &contour;
            }
            if (contour.size() < 3) continue;
            auto& mask = masks.emplace_back();
            mask.reserve(contour.size());
            for (const auto& p : contour) mask.push_back(maskPoint(p.x, p.y));
        }
        const auto drawSurface = [&](const Surface& surface, Rect overrideBounds = {}) {
            const auto& image = surface.path;
            const auto& key = surface.key;
            if ((!include.empty() && !include.contains(key)) || (!exclude.empty() && exclude.contains(key))) return;
            auto binding = bindings.end();
            if (!bindings.empty()) {
                const auto alias = textureAliases.find(key);
                binding = bindings.find(alias == textureAliases.end() ? key : alias->second);
                if (!bindingName.empty()) {
                    std::string bound(bindingName);
                    const auto allowed = slotTextures.find(bound);
                    if (allowed == slotTextures.end() || std::ranges::find(allowed->second, key) != allowed->second.end()) {
                        if (allowed != slotTextures.end() && allowed->second.front() != key) bound += "_" + key;
                        if (auto it = bindings.find(bound); it != bindings.end()) binding = it;
                    }
                }
                if (!pathBinding.empty())
                    if (const auto replacement = bindings.find(std::string(pathBinding)); replacement != bindings.end())
                        binding = replacement;
            }
            if (binding == bindings.end() ? surface.runtime : binding->second.path.empty()) return;
            const auto path = binding == bindings.end() ? std::string_view(image) : binding->second.path;
            Rect geometry = overrideBounds.w > 0 ? overrideBounds : surface.bounds;
            if (binding != bindings.end() && binding->second.targetRect.w > 0) geometry = binding->second.targetRect;
            const bool crop = binding == bindings.end() && surface.source.w > 0;
            Vec2 original{};
            if (!surface.runtime && !image.empty() && (geometry.w <= 0 || crop)) original = render.imageSize(image);
            if (geometry.w <= 0) geometry = {0, 0, original.x, original.y};
            Rect uv{0, 0, 1, 1};
            if (crop && original.x > 0 && original.y > 0)
                uv = {surface.source.x / original.x, surface.source.y / original.y,
                      surface.source.w / original.x, surface.source.h / original.y};
            auto tint = multiply, offset = add;
            if (image.empty() && binding == bindings.end()) {
                const std::array rgba{surface.color.r, surface.color.g, surface.color.b, surface.color.a};
                for (size_t i = 0; i < rgba.size(); ++i) tint[i] *= rgba[i];
            }
            if (binding != bindings.end()) {
                const auto& [replacement, sourceRect, target, mul, extra] = binding->second;
                if (sourceRect.w > 0) {
                    const auto size = render.imageSize(replacement);
                    uv = {sourceRect.x / size.x, sourceRect.y / size.y, sourceRect.w / size.x, sourceRect.h / size.y};
                }
                const Channels tintValues{mul.r, mul.g, mul.b, mul.a}, addValues{extra.r, extra.g, extra.b, extra.a};
                for (size_t i = 0; i < 4; ++i) {
                    offset[i] += tint[i] * addValues[i];
                    tint[i] *= tintValues[i];
                }
            }

            const auto [x, y, w, h] = geometry;
            if (w <= 0 || h <= 0) return;
            const std::array positions{project(x,y), project(x+w,y), project(x+w,y+h), project(x,y+h)};
            const auto minmax = std::ranges::minmax_element(positions, {}, [](auto& p) { return p[2]; });
            if ((*minmax.max)[2] <= 0) return;
            const bool nearClipped = (*minmax.min)[2] <= 0;
            const double depth = nearClipped ? (*minmax.max)[2] : projection[8];
            if (!nearClipped) {
                const auto outside = [&](size_t axis, double boundary, auto compare) {
                    return std::ranges::all_of(positions, [&](const auto& p) { return compare(p[axis], boundary * p[2]); });
                    };
                    if (outside(0,0.,std::less_equal{}) || outside(0,movie.size.x,std::greater_equal{}) ||
                        outside(1,0.,std::less_equal{}) || outside(1,movie.size.y,std::greater_equal{})) return;
                }
                std::array<float, 9> m;
                for (size_t i = 0; i < m.size(); ++i) m[i] = float(projection[i] / depth);
                const auto corner = [&](float cx, float cy) {
                    const float weight = m[6] * cx + m[7] * cy + m[8];
                    return onPlane({sx * (m[0] * cx + m[1] * cy + m[2]) + bounds.x * weight,
                                    sy * (m[3] * cx + m[4] * cy + m[5]) + bounds.y * weight, weight});
                };
                const std::array corners{corner(x,y), corner(x+w,y), corner(x+w,y+h), corner(x,y+h)};
                if (nearClipped) {
                    auto& mask = masks.emplace_back();
                    const auto near = depth * .0001;
                    const std::array points{Vec2{x,y}, Vec2{x+w,y}, Vec2{x+w,y+h}, Vec2{x,y+h}};
                    for (size_t i = 0; i < points.size(); ++i) {
                        const auto j = (i + 1) % points.size();
                        const auto a = positions[i][2] - near, b = positions[j][2] - near;
                        if (a >= 0) mask.push_back(maskPoint(points[i].x, points[i].y));
                        if ((a < 0) != (b < 0)) {
                            const double fraction = a / (a - b);
                            mask.push_back(maskPoint(points[i].x + (points[j].x - points[i].x) * fraction,
                                                     points[i].y + (points[j].y - points[i].y) * fraction));
                        }
                    }
                }
                const auto blendMode = blend == 8 ? Blend::Add : blend == 3 ? Blend::Multiply : Blend::Alpha;
                tint[3] *= opacity;
                offset[3] *= opacity;
                render.projected(path, corners, color(tint), color(offset), blendMode, masks, uv, surface.nearest,
                                 {float(shift[0]), float(shift[1]), float(shift[2])}, filter);
                if (nearClipped) masks.pop_back();
        };
        if (!path.empty()) {
            if (const auto slot = textureInstances.find(path); slot != textureInstances.end()) {
                pathBinding = slot->second;
                const Surface surface{.path = {}, .key = {}, .runtime = true};
                drawSurface(surface);
                masks.resize(inheritedMasks);
                return;
            }
        }
        // AFP paints a simple filled contour in the placed object's depth slot.
        // Contours in mask mode clip the placed object's usual display list.
        if (paintFill) {
            if (fillContour && fillContour->size() >= 3) {
                if (instance.solid) {
                    Surface solid;
                    solid.color = *instance.solid;
                    drawSurface(solid, {0, 0, instance.solidSize.x, instance.solidSize.y});
                } else {
                    drawSurface(*instance.bitmap, {0, 0, instance.bitmapSize.x, instance.bitmapSize.y});
                }
            }
        } else if (instance.timeline) {
            for (const auto& child : instance.children) {
                if (&instance == &root && !rootInstance.empty() && child->name != rootInstance) continue;
                // A camera belongs to its clip. Project that clip's children locally
                // before applying its placement in the containing camera's view.
                draw(render, *child, instance.camera ? Identity : matrix, instance.camera ? false : perspective,
                     multiply, add, shift, blend, masks, bounds, bindings, bindingName, plane, opacity,
                     filter, parentPath, pathBinding, instance.camera ? &*instance.camera : camera,
                     instance.camera ? &projection : outerProjection);
            }
        } else if (instance.projection) {
            for (const auto& surface : instance.source->surfaces) drawSurface(surface);
        }
        masks.resize(inheritedMasks);
    }
};

AfpMovie::AfpMovie(std::string_view folder, std::string_view movie)
    : impl_(std::make_unique<Impl>(std::make_shared<Movie>(folder, movie))) {}
AfpMovie::AfpMovie(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AfpMovie::~AfpMovie() = default;
AfpMovie::AfpMovie(AfpMovie&&) noexcept = default;
AfpMovie& AfpMovie::operator=(AfpMovie&&) noexcept = default;

AfpMovie AfpMovie::fork() const {
    auto copy = std::make_unique<Impl>(impl_->parsedMovie);
    copy->include = impl_->include;
    copy->exclude = impl_->exclude;
    copy->textureSlots = impl_->textureSlots;
    copy->textureAliases = impl_->textureAliases;
    copy->texturePaths = impl_->texturePaths;
    copy->textureInstances = impl_->textureInstances;
    copy->replacements = impl_->replacements;
    copy->slotTextures = impl_->slotTextures;
    copy->hiddenInstances = impl_->hiddenInstances;
    copy->dimmedInstances = impl_->dimmedInstances;
    copy->rootInstance = impl_->rootInstance;
    copy->brightness = impl_->brightness;
    return AfpMovie(std::move(copy));
}

void AfpMovie::start(std::string_view label, double time) {
    start(impl_->movie.root.labels.at(label), time);
}

void AfpMovie::start(size_t frame, double time) {
    impl_->root = {};
    impl_->root.timeline = &impl_->movie.root;
    impl_->parameters.clear();
    impl_->root.requested = std::min(frame + 1, impl_->movie.root.frames.size());
    impl_->root.skipActionsBeforeLabel = true;
    impl_->tick = 0;
    impl_->advance(1);
    impl_->startTime = time;
    impl_->rate = 1;
    impl_->started = true;
}

void AfpMovie::stop() { impl_->started = false; }
bool AfpMovie::started() const { return impl_->started; }
void AfpMovie::holdRoot() { impl_->root.playing = false; }

void AfpMovie::configure(const nlohmann::json& options) {
    impl_->include = options.value("include", std::unordered_set<std::string>{});
    impl_->exclude = options.value("exclude", std::unordered_set<std::string>{});
    impl_->hiddenInstances = options.value("hidden_instances", std::unordered_set<std::string>{});
    impl_->dimmedInstances = options.value("dimmed_instances", std::unordered_set<std::string>{});
    impl_->brightness = options.value("brightness", 1.);
    impl_->rootInstance = options.value("root_instance", std::string{});
    impl_->textureAliases = options.value("texture_aliases", std::unordered_map<std::string, std::string>{});
    impl_->texturePaths = options.value("texture_paths", std::unordered_map<std::string, std::string>{});
    impl_->textureInstances = options.value("texture_instances", std::unordered_map<std::string, std::string>{});
    if (auto slots = options.find("texture_slots"); slots != options.end()) {
        if (slots->is_array()) impl_->textureSlots = slots->get<std::unordered_set<std::string>>();
        else impl_->slotTextures = slots->get<decltype(impl_->slotTextures)>();
    }
    if (const auto replacements = options.find("replace_instances"); replacements != options.end()) {
        std::unordered_map<std::string, std::shared_ptr<const Movie>> movies;
        for (const auto& [path, entry] : replacements->items()) {
            const auto& name = entry.at(0).get_ref<const std::string&>();
            const auto& symbol = entry.at(1).get_ref<const std::string&>();
            auto& movie = movies[name];
            if (!movie) movie = std::make_shared<Movie>(impl_->movie.folder, name, 1);
            const auto& source = movie->definition(movie->exports.at(symbol));
            impl_->replacements.insert_or_assign(path.starts_with('/') ? path : '/' + path,
                Impl::Replacement{movie, &source, source.owner->reachableImages(*source.timeline)});
        }
    }
}

void AfpMovie::setRate(double rate, double time) {
    if (!impl_->started || rate <= 0) return;
    impl_->advance(impl_->cueTick(time));
    impl_->startTime = time - (time - impl_->startTime) * impl_->rate / rate;
    impl_->rate = rate;
}

void AfpMovie::cue(std::string_view path, std::string_view label, double time) {
    cue(path, label, time, false);
}

void AfpMovie::cue(std::string_view path, std::string_view label, double time, bool rewindBackward) {
    if (!impl_->started) return;
    impl_->advance(impl_->cueTick(time));
    const auto apply = [&](auto&& self, Instance& instance, std::string_view target) -> void {
        if (target.empty() || target == "/") {
            const auto found = instance.timeline->labels.find(label);
            if (found == instance.timeline->labels.end()) return;
            const auto frame = found->second;
            if ((!frame && label == "non") || (rewindBackward && frame + 1 < instance.frame)) instance.rewind();
            // A loop may have moved the playhead behind the last applied tags.
            // Restore a label's inherited pose without restarting child animations.
            else if (instance.timeline->frames.at(frame).first < instance.tagCursor) instance.tagCursor = 0;
            instance.frame = std::min(frame, instance.timeline->frames.size());
            instance.requested = std::min(frame + 1, instance.timeline->frames.size());
            instance.skipActionsBeforeLabel = true;
            instance.playing = true;
            return;
        }
        const auto slash = target.find('/');
        for (auto& child : instance.children)
            if (child->timeline && child->name == target.substr(0, slash))
                self(self, *child, slash == target.npos ? std::string_view{} : target.substr(slash + 1));
    };
    apply(apply, impl_->root, path);
}

void AfpMovie::parameter(std::string_view path, std::string_view fromLabel, std::string_view toLabel,
                         double start, double end, double from, double to) {
    if (!impl_->started) return;
    const auto first = impl_->cueTick(start);
    impl_->advance(first);
    std::erase_if(impl_->parameters, [&](const auto& parameter) { return parameter.path == path; });
    impl_->parameters.push_back({std::string(path), std::string(fromLabel), std::string(toLabel),
                                first, std::max(first, impl_->cueTick(end)), from, to});
}

void AfpMovie::applyCues() {
    while (impl_->dirty(impl_->root)) impl_->step(impl_->root, true);
}

void AfpMovie::advance(double time) {
    if (!impl_->started) return;
    const auto frame = size_t(std::floor(std::max(0.0, time - impl_->startTime) * impl_->rate * impl_->movie.fps / 1000));
    impl_->advance(frame + 1);
}

double AfpMovie::fps() const { return impl_->movie.fps; }
Vec2 AfpMovie::size() const { return impl_->movie.size; }
size_t AfpMovie::frames() const { return impl_->movie.root.frames.size(); }
Render::AfpAnchor AfpMovie::anchor(std::string_view name) const {
    const auto* instance = impl_->find(name);
    return instance ? Render::AfpAnchor{{float(instance->matrix[9]), float(instance->matrix[10])}, float(instance->multiply[3])}
                    : Render::AfpAnchor{};
}

std::optional<size_t> AfpMovie::label(std::string_view name) const {
    const auto& labels = impl_->movie.root.labels;
    if (const auto label = labels.find(name); label != labels.end()) return label->second;
    return std::nullopt;
}

void AfpMovie::queueTextures(Render& render, bool stage, bool deferred) const {
    const auto queue = [&](const std::string& image) {
        if (deferred) render.queueImage(image, stage);
        else render.imageSize(image);
    };
    for (const auto& image : impl_->movie.images) queue(image);
    for (const auto& [_, replacement] : impl_->replacements)
        for (const auto& image : replacement.textures) queue(image);
}

void AfpMovie::draw(Render& render, Rect bounds, const Bindings& bindings,
                    const std::array<Render::Projected, 3>* plane, float opacity) const {
    if (!impl_->started) return;
    std::vector<Mask> masks;
    if (plane) {
        const auto& [o,r,b] = *plane;
        const Render::Projected opposite{r.x+b.x-o.x, r.y+b.y-o.y, r.w+b.w-o.w};
        auto& mask = masks.emplace_back();
        for (const auto& p : {o,r,opposite,b}) mask.push_back({p.x/p.w,p.y/p.w});
    } else render.pushClip(bounds);
    const auto brightness = impl_->brightness;
    impl_->draw(render, impl_->root, Identity, false, {brightness, brightness, brightness, 1}, {}, {}, 0, masks, bounds,
                bindings, {}, plane, opacity,
                nullptr, {}, {});
    if (!plane) render.popClip();
}

}
