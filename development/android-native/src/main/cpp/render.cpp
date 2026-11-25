#include "app.h"
#include "afp.h"
#include "stage_archive.h"

#include <SDL3/SDL.h>
#include <GLES3/gl32.h>
#include <RmlUi/Core.h>
#include <RmlUi_Renderer_GL3.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include <webp/decode.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <stdexcept>

unsigned int sdvxRmlDrawTarget = 0;
unsigned int sdvxRmlOverlayTexture = 0;

namespace sdvx {
namespace {
struct FontSpec {
    const char* path;
    int faceIndex;
    const char* family;
    bool fallback = false;
};

// Face indices are within each TTC. The original font slots are 0, 11, 12, 6, 7, 14, and 13.
constexpr std::array FontSpecs{
    FontSpec{"font/0003.ttc", 1, "Nabla", true},
    FontSpec{"font/0001.ttf", 0, "NablaDisplay"},
    FontSpec{"font/DF-HeiSeiMaruGothic-W4.ttc", 1, "NablaMaruW4"},
    FontSpec{"font/0005.ttf", 0, "NablaDigital"},
    FontSpec{"font/0000.ttc", 1, "NablaTest"},
    FontSpec{"font/WKMRGOTH.TTF", 0, "NablaKorean", true},
    FontSpec{"font/DF-HeiSeiMaruGothic-W4.ttc", 0, "NablaMaruW4Fixed"},
};

struct Vertex {
    float x, y, w, u, v, clipZ = 0, alpha = 1;
    bool operator==(const Vertex&) const = default;
};
struct QuadInstance { std::array<Vertex, 4> corners; Color multiply, add; };

float edgeDistance(Vec2 a, Vec2 b, const Vertex& p) {
    return (b.x - a.x) * (p.y - a.y * p.w) - (b.y - a.y) * (p.x - a.x * p.w);
}

float convexWinding(std::span<const Vec2> points) {
    if (points.size() < 3) return 0;
    float winding = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size()], c = points[(i + 2) % points.size()];
        const float turn = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (turn == 0) continue;
        if (winding * turn < 0) return 0;
        winding = std::copysign(1.f, turn);
    }
    if (!winding) return 0;
    // A self-crossing contour can turn the same way at every point.
    // A convex one also keeps all vertices on the inside of each edge.
    for (size_t i = 0; i < points.size(); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size()];
        for (const auto p : points) {
            // Edge endpoints are on the edge by definition. On ARM, FMA can
            // leave a negative rounding residual when testing b against a->b.
            if ((p.x == a.x && p.y == a.y) || (p.x == b.x && p.y == b.y)) continue;
            if (winding * ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) < 0) return 0;
        }
    }
    return winding;
}

// Clip the homogeneous vertices so AFP masks keep perspective-correct UVs.
void clipPolygon(std::vector<Vertex>& result, std::vector<Vertex>& scratch,
                 std::span<const Vec2> mask, float winding) {
    for (size_t side = 0; side < mask.size() && !result.empty(); ++side) {
        const auto a = mask[side], b = mask[(side + 1) % mask.size()];
        const auto outside = [&](const Vertex& p) { return winding * edgeDistance(a, b, p) < 0; };
        if (std::ranges::none_of(result, outside)) continue;
        Vertex previous = result.back();
        float previousDistance = winding * edgeDistance(a, b, previous);
        scratch.clear();
        for (const Vertex current : result) {
            const float distance = winding * edgeDistance(a, b, current);
            if ((distance > 0 && previousDistance < 0) || (distance < 0 && previousDistance > 0)) {
                const float t = previousDistance / (previousDistance - distance);
                scratch.push_back({
                    std::lerp(previous.x, current.x, t), std::lerp(previous.y, current.y, t),
                    std::lerp(previous.w, current.w, t), std::lerp(previous.u, current.u, t),
                    std::lerp(previous.v, current.v, t),
                    std::lerp(previous.clipZ, current.clipZ, t),
                    std::lerp(previous.alpha, current.alpha, t)
                });
            }
            if (distance >= 0) scratch.push_back(current);
            previous = current;
            previousDistance = distance;
        }
        result.swap(scratch);
    }
}
struct Pixels {
    int w = 0, h = 0;
    std::unique_ptr<uint8_t, void (*)(void*)> rgba{nullptr, stbi_image_free};
    std::vector<uint8_t> archivePixels;
    uint8_t* data() { return archivePixels.empty() ? rgba.get() : archivePixels.data(); }
    const uint8_t* data() const { return archivePixels.empty() ? rgba.get() : archivePixels.data(); }
};

Pixels decodeImage(std::string_view path) {
    Pixels image;
    if (const auto* entry = stageArchiveEntry(path); entry && entry->width) {
        image.w = entry->width;
        image.h = entry->height;
        image.archivePixels = readStagePixels(*entry);
        return image;
    }
    auto data = readAsset(path);
    if (data.empty()) return {};
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    if (path.ends_with(".webp")) {
        image.rgba = {WebPDecodeRGBA(bytes, data.size(), &image.w, &image.h), WebPFree};
    } else {
        image.rgba.reset(stbi_load_from_memory(bytes, data.size(), &image.w, &image.h, nullptr, 4));
    }
    return image;
}

GLuint makeTexture(int width, int height, const void* data) {
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return id;
}

GLuint compileShader(GLenum type, const char* body) {
#ifdef SDVX_EMULATOR_GLES31
    constexpr const char* version = "#version 310 es\n";
#else
    constexpr const char* version = "#version 320 es\n";
#endif
    GLuint shader = glCreateShader(type);
    const char* sources[] = {version, body};
    glShaderSource(shader, 2, sources, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(log);
    }
    return shader;
}

struct LaserGlow {
    struct Pass {
        GLuint program = 0;
        GLint texel = -1, combine = -1;
    };
    struct Level {
        GLuint vertical = 0, diagonal = 0, result = 0;
        GLuint blurTarget = 0, combineTarget = 0;
    };
    GLuint source = 0, sourceTarget = 0, composite = 0;
    GLint gain = -1;
    Pass blur;
    std::array<Level, 3> levels;

    LaserGlow(GLuint canvas, GLuint stencil) {
        const auto vertex = compileShader(GL_VERTEX_SHADER, R"(
            out vec2 uv;
            void main() {
                uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
                gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
            }
        )");
        const auto link = [vertex](const char* code) {
            const auto fragment = compileShader(GL_FRAGMENT_SHADER, code);
            const auto program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            glDeleteShader(fragment);
            GLint linked;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) throw std::runtime_error("Could not link laser glow program");
            return program;
        };
        // HexagonalBlurEffectMultiStage, 2026101300 HLSL at 18092cc10/18092d250.
        // D3D's texture Y points down. The first pass writes premultiplied RGB.
        blur.program = link(R"(
            precision highp float;
            in vec2 uv;
            uniform sampler2D input0, input1;
            uniform vec2 texel;
            uniform bool combine;
            layout(location=0) out vec4 output0;
            layout(location=1) out vec4 output1;

            vec3 weightedBlur(sampler2D image, vec2 direction, float coc) {
                if (coc == 0.0) return vec3(0.0);
                vec2 step = direction * texel * coc;
                vec3 rgb = vec3(0.0);
                float weight = 0.0;
                for (int i = 0; i < 4; ++i) {
                    vec4 pixel = texture(image, uv + (float(i) + 0.5) * step);
                    rgb += pixel.rgb * pixel.a;
                    weight += pixel.a * pixel.a;
                }
                return weight > 0.0 ? rgb * (coc / weight) : vec3(0.0);
            }

            void main() {
                const vec2 right = vec2(0.8660254, 0.5);
                float coc = texture(input0, uv).a;
                if (combine) {
                    // Both MRT outputs carry the same CoC.
                    vec3 vertical = weightedBlur(input0, right, coc);
                    vec3 diagonal = weightedBlur(input1, vec2(-right.x, right.y), coc);
                    output0 = vec4((vertical + diagonal) * 0.5, 1.0);
                } else {
                    vec3 vertical = weightedBlur(input0, vec2(0.0, -1.0), coc);
                    vec3 diagonal = weightedBlur(input0, right, coc);
                    output0 = vec4(clamp(vertical, 0.0, 1.0) * coc, coc);
                    output1 = vec4(clamp(diagonal + vertical, 0.0, 1.0) * coc, coc);
                }
            }
        )");
        composite = link(R"(
            precision highp float;
            in vec2 uv;
            uniform sampler2D input0, input1, input2;
            uniform float gain;
            out vec4 outputColor;
            void main() {
                vec3 rgb = texture(input0, uv).rgb + texture(input1, uv).rgb + texture(input2, uv).rgb;
                outputColor = vec4(clamp(rgb, 0.0, 1.0) * gain, 1.0);
            }
        )");
        glDeleteShader(vertex);
        blur.texel = glGetUniformLocation(blur.program, "texel");
        blur.combine = glGetUniformLocation(blur.program, "combine");
        glUseProgram(blur.program);
        glUniform1i(glGetUniformLocation(blur.program, "input1"), 1);
        glUseProgram(composite);
        glUniform1i(glGetUniformLocation(composite, "input1"), 1);
        glUniform1i(glGetUniformLocation(composite, "input2"), 2);
        gain = glGetUniformLocation(composite, "gain");
        GLint previous;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
        source = makeTexture(Width, Height, nullptr);
        glGenFramebuffers(1, &sourceTarget);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sourceTarget);
        attach(canvas, source);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
        for (size_t i = 0; i < levels.size(); ++i) {
            const int width = int(Width) >> i, height = int(Height) >> i;
            auto& level = levels[i];
            level.vertical = makeTexture(width, height, nullptr);
            level.diagonal = makeTexture(width, height, nullptr);
            level.result = makeTexture(width, height, nullptr);
            glGenFramebuffers(1, &level.blurTarget);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, level.blurTarget);
            attach(level.vertical, level.diagonal);
            glGenFramebuffers(1, &level.combineTarget);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, level.combineTarget);
            attach(level.result, 0);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
    }

    ~LaserGlow() {
        for (const auto& level : levels) {
            const GLuint textures[]{level.vertical, level.diagonal, level.result};
            glDeleteTextures(3, textures);
            const GLuint targets[]{level.blurTarget, level.combineTarget};
            glDeleteFramebuffers(2, targets);
        }
        glDeleteTextures(1, &source);
        glDeleteFramebuffers(1, &sourceTarget);
        glDeleteProgram(blur.program);
        glDeleteProgram(composite);
    }

    static void attach(GLuint first, GLuint second) {
        const GLenum buffers[]{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, first, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, second, 0);
        glDrawBuffers(second ? 2 : 1, buffers);
    }

    void draw(GLuint scene, GLuint input, float strength, float renderMax, float renderAdd) const {
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(blur.program);
        int sourceWidth = Width, sourceHeight = Height;
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto& level = levels[i];
            const int width = int(Width) >> i, height = int(Height) >> i;
            glViewport(0, 0, width, height);
            glUniform2f(blur.texel, strength / sourceWidth, strength / sourceHeight);
            glUniform1i(blur.combine, false);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, level.blurTarget);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, input);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            glUniform1i(blur.combine, true);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, level.combineTarget);
            glBindTexture(GL_TEXTURE_2D, level.vertical);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, level.diagonal);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            input = level.result;
            sourceWidth = width;
            sourceHeight = height;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene);
        glViewport(0, 0, Width, Height);
        glUseProgram(composite);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        for (size_t i = 0; i < levels.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, levels[i].result);
        }
        // 180397250 composites the saturated sum using MAX then ADD.
        // The cabinet truncates 0.375 and 0.25 to 8-bit vertex colors.
        glBlendEquationSeparate(GL_MAX, GL_FUNC_ADD);
        glUniform1f(gain, renderMax);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBlendEquation(GL_FUNC_ADD);
        glUniform1f(gain, renderAdd);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
    }
};

class AssetFiles : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override {
        return reinterpret_cast<Rml::FileHandle>(SDL_IOFromFile(path.c_str(), "rb"));
    }
    void Close(Rml::FileHandle file) override { SDL_CloseIO(reinterpret_cast<SDL_IOStream*>(file)); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        return SDL_ReadIO(reinterpret_cast<SDL_IOStream*>(file), buffer, size);
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        return SDL_SeekIO(reinterpret_cast<SDL_IOStream*>(file), offset, SDL_IOWhence(origin)) >= 0;
    }
    size_t Tell(Rml::FileHandle file) override { return SDL_TellIO(reinterpret_cast<SDL_IOStream*>(file)); }
};

class UiSystem : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return SDL_GetTicksNS() / 1.0e9; }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) SDL_Log("RmlUi: %s", message.c_str());
        return true;
    }
};

class UiRenderer : public RenderInterface_GL3 {
public:
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        auto image = decodeImage(source);
        if (!image.data()) return {};
        const Rml::Span pixels{image.data(), size_t(image.w) * image.h * 4};
        for (size_t i = 0; i < pixels.size(); i += 4) {
            for (int c = 0; c < 3; ++c) pixels[i + c] = (pixels[i + c] * pixels[i + 3] + 127) / 255;
        }
        dimensions = {image.w, image.h};
        return GenerateTexture({pixels.data(), pixels.size()}, dimensions);
    }
};

Rect intersection(Rect a, Rect b) {
    float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - y)};
}

float cross(Vec2 a, Vec2 b, Vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

std::vector<Vertex> triangles(std::span<const Vec2> points) {
    std::vector<Vertex> vertices;
    if (points.size() < 3) return vertices;
    std::vector<size_t> ring;
    float signedArea = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec2 a = points[i], b = points[(i + 1) % points.size()];
        if (a.x == b.x && a.y == b.y) continue;
        ring.push_back(i);
        signedArea += a.x * b.y - a.y * b.x;
    }
    float winding = signedArea >= 0 ? 1 : -1;
    while (ring.size() > 2) {
        bool cut = false;
        for (size_t i = 0; i < ring.size(); ++i) {
            size_t p = (i + ring.size() - 1) % ring.size(), n = (i + 1) % ring.size();
            Vec2 a = points[ring[p]], b = points[ring[i]], c = points[ring[n]];
            if (cross(a, b, c) * winding <= 0) continue;
            bool contains = false;
            for (size_t j = 0; j < ring.size(); ++j) {
                if (j == p || j == i || j == n) continue;
                Vec2 v = points[ring[j]];
                if (cross(a, b, v) * winding > 0 && cross(b, c, v) * winding > 0 && cross(c, a, v) * winding > 0) {
                    contains = true;
                    break;
                }
            }
            if (contains) continue;
            for (Vec2 v : {a, b, c}) vertices.push_back({v.x, v.y, 1, 0, 0});
            ring.erase(ring.begin() + i);
            cut = true;
            break;
        }
        if (!cut) break;
    }
    return vertices;
}
}

struct Render::Impl {
    struct TextureHash {
        using is_transparent = void;
        size_t operator()(std::string_view path) const { return std::hash<std::string_view>{}(path); }
    };
    struct Material {
        GLuint texture;
        Color multiply, add;
        Blend blend;
        bool premultiplied;
        bool nearest;
        bool glow;
        std::array<float, 3> hsl;
        std::optional<ColorMatrix> filter;
        bool operator==(const Material&) const = default;
    };
    struct Draw {
        Material material;
        Rect clip;
        GLint first;
        GLsizei count;
        bool instanced;
    };
    struct Texture {
        GLuint id = 0;
        int w = 0, h = 0;
        uint64_t used = 0;
        bool premultiplied = false;
        bool stage = false;
    };
    struct Font { std::string bytes; FT_Face face = nullptr; };
    struct Glyph {
        GLuint page = 0;
        Rect uv{};
        float advance = 0, left = 0, top = 0, w = 0, h = 0;
    };
    SDL_Window* window;
    int windowW = 1, windowH = 1;
    float canvasW = Width, canvasH = Height;
    int viewX = 0, viewY = 0, viewW = Width, viewH = Height;
    GLuint vao = 0, vbo = 0, quadVao = 0, quadVbo = 0, white = 0;
    struct SpriteProgram {
        GLuint id = 0;
        GLint multiply, add, premultiplied, glow, canvasSize, hslShift;
        GLint matrixEnabled, matrixRows, matrixBias;
    };
    std::array<SpriteProgram, 4> spritePrograms;
    std::array<GLuint, 2> samplers{};
    GLuint framebuffer = 0, canvas = 0, stencilBuffer = 0;
    struct StageLayerTarget { GLuint framebuffer = 0, stencil = 0; };
    std::vector<StageLayerTarget> stageLayerTargets;
    size_t stageLayerDepth = 0;
    GLuint canvasCopyTarget = 0;
    GLuint laserGlowTarget = 0;
    GLuint stagePostProgram = 0, stageColorProgram = 0, stageColorTarget = 0;
    GLuint stagePostInput = 0, stageFilterTarget = 0;
    GLint postEffect = -1, postCenter = -1, postOffset = -1, postCrt = -1;
    GLint stageColorMode = -1, stageColorStrength = -1;
    GLuint tunnelNoiseProgram = 0, tunnelTarget = 0, tunnelImage = 0, tunnelNoiseRows = 0;
    GLint tunnelNoiseSeed = -1;
    std::unique_ptr<LaserGlow> laserGlow;
    bool drawingLaserGlow = false;
    uint64_t frame = 0;
    bool uiResized = false;
    size_t textureBytes = 0;
    uint64_t nextTextureTrim = 0;
    int stencilIndex = 0;
    std::unordered_map<std::string, Texture, TextureHash, std::equal_to<>> textures;
    std::deque<std::string> queuedImages;
    std::deque<std::string> stageImages;
    struct ImageLoad {
        std::string path;
        bool stage;
        std::future<Pixels> pixels;
    };
    std::vector<ImageLoad> decodingImages;
    std::vector<Rect> clips;
    std::vector<Vertex> vertices;
    std::vector<QuadInstance> quads;
    std::vector<Vertex> clipVertices, clipScratch, clipTriangles;
    std::vector<float> maskWindings;
    std::vector<std::vector<Vec2>> stencilMasks;
    GLint stencilTarget = -1;
    bool stencilActive = false;
    Rect stencilClip{};
    std::array<float, 6> stencilTransform{};
    std::vector<Draw> draws;
    std::vector<std::array<float, 6>> transforms;
    std::optional<std::span<const Vec2>> playfieldMask;
    std::vector<Vertex> playfieldPolygon, playfieldScratch, playfieldTriangles;
    AfpLibrary afp;
    FT_Library freetype = nullptr;
    std::array<Font, FontSpecs.size()> fonts;
    std::unordered_map<uint64_t, Glyph> glyphs;
    std::vector<GLuint> pages;
    int glyphX = 0, glyphY = 0, rowHeight = 0;
    static constexpr int AtlasSize = 2048;
    AssetFiles files;
    UiSystem system;
    std::unique_ptr<UiRenderer> uiRenderer;
    Rml::Context* context = nullptr;

    explicit Impl(SDL_Window* target) : window(target) {
        const char* vertex = R"(
            #if INSTANCED
            layout(location=0) in vec3 position0;
            layout(location=1) in vec3 position1;
            layout(location=2) in vec3 position2;
            layout(location=3) in vec3 position3;
            layout(location=4) in vec4 uvClip0;
            layout(location=5) in vec4 uvClip1;
            layout(location=6) in vec4 uvClip2;
            layout(location=7) in vec4 uvClip3;
            layout(location=8) in vec4 instanceMultiply;
            layout(location=9) in vec4 instanceAdd;
            flat out vec4 multiplyColor;
            flat out vec4 addColor;
            #else
            layout(location=0) in vec3 xyw;
            layout(location=1) in vec2 uv;
            layout(location=2) in vec2 clipZAndAlpha;
            #endif
            out vec2 tex;
            out float vertexAlpha;
            uniform vec2 canvasSize;
            void main() {
                #if INSTANCED
                int corner = gl_VertexID < 3 ? gl_VertexID : gl_VertexID == 3 ? 0 : gl_VertexID - 2;
                vec3 xyw = corner == 0 ? position0 : corner == 1 ? position1 : corner == 2 ? position2 : position3;
                vec4 attributes = corner == 0 ? uvClip0 : corner == 1 ? uvClip1 : corner == 2 ? uvClip2 : uvClip3;
                vec2 uv = attributes.xy;
                vec2 clipZAndAlpha = attributes.zw;
                multiplyColor = instanceMultiply;
                addColor = instanceAdd;
                #endif
                gl_Position = vec4(xyw.x * 2.0 / canvasSize.x - xyw.z, xyw.z - xyw.y * 2.0 / canvasSize.y,
                                   clipZAndAlpha.x, xyw.z);
                tex = uv;
                vertexAlpha = clipZAndAlpha.y;
            }
        )";
        const char* fragment = R"(
            precision highp float;
            in vec2 tex;
            in float vertexAlpha;
            uniform sampler2D image;
            #if INSTANCED
            flat in vec4 multiplyColor;
            flat in vec4 addColor;
            #else
            uniform vec4 multiplyColor;
            uniform vec4 addColor;
            #endif
            uniform bool premultiplied;
            uniform bool glow;
            uniform vec3 hslShift;
            uniform bool matrixEnabled;
            uniform vec4 matrixRows[4];
            uniform vec4 matrixBias;
            layout(location=0) out vec4 outputColor;
            layout(location=1) out vec4 outputGlow;
            #if COLOR_EFFECTS
            vec3 shiftHsl(vec3 rgb) {
                float high = max(max(rgb.r, rgb.g), rgb.b);
                float low = min(min(rgb.r, rgb.g), rgb.b);
                float light = (high + low) * 0.5;
                float range = high - low;
                float hue = 0.0;
                float saturation = 0.0;
                if (range > 0.0) {
                    saturation = range / (1.0 - abs(2.0 * light - 1.0));
                    hue = (high == rgb.r ? (rgb.g - rgb.b) / range :
                           high == rgb.g ? 2.0 + (rgb.b - rgb.r) / range : 4.0 + (rgb.r - rgb.g) / range) / 6.0;
                }
                hue = fract(hue + hslShift.x);
                saturation = clamp(saturation + hslShift.y, 0.0, 1.0);
                light = clamp(light + hslShift.z, 0.0, 1.0);
                vec3 channels = clamp(abs(fract(hue + vec3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
                return light + (channels - 0.5) * (1.0 - abs(2.0 * light - 1.0)) * saturation;
            }
            #endif
            void main() {
                vec4 pixel = texture(image, tex);
                if (premultiplied) pixel.rgb = pixel.a > 0.0 ? pixel.rgb / pixel.a : vec3(0.0);
                #if COLOR_EFFECTS
                if (matrixEnabled) {
                    pixel = vec4(dot(matrixRows[0], pixel), dot(matrixRows[1], pixel),
                                 dot(matrixRows[2], pixel), dot(matrixRows[3], pixel)) + matrixBias;
                    pixel = clamp(pixel, 0.0, 1.0);
                }
                #endif
                vec4 color = clamp(pixel * multiplyColor + addColor, 0.0, 1.0);
                color.a *= vertexAlpha;
                if (color.a == 0.0 || (glow && color.a < 0.01)) discard;
                #if COLOR_EFFECTS
                if (any(notEqual(hslShift, vec3(0.0)))) color.rgb = shiftHsl(color.rgb);
                #endif
                outputColor = vec4(color.rgb * color.a, color.a);
                outputGlow = glow ? outputColor : vec4(0.0);
            }
        )";
        for (size_t i = 0; i < spritePrograms.size(); ++i) {
            auto& shader = spritePrograms[i];
            const auto prefix = std::string(i & 1 ? "#define COLOR_EFFECTS 1\n" : "#define COLOR_EFFECTS 0\n") +
                (i & 2 ? "#define INSTANCED 1\n" : "#define INSTANCED 0\n");
            const auto vs = compileShader(GL_VERTEX_SHADER, (prefix + vertex).c_str());
            const auto fs = compileShader(GL_FRAGMENT_SHADER, (prefix + fragment).c_str());
            shader.id = glCreateProgram();
            glAttachShader(shader.id, vs);
            glAttachShader(shader.id, fs);
            glLinkProgram(shader.id);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint linked = 0;
            glGetProgramiv(shader.id, GL_LINK_STATUS, &linked);
            if (!linked) throw std::runtime_error("Could not link GLES sprite program");
            shader.multiply = glGetUniformLocation(shader.id, "multiplyColor");
            shader.canvasSize = glGetUniformLocation(shader.id, "canvasSize");
            shader.add = glGetUniformLocation(shader.id, "addColor");
            shader.premultiplied = glGetUniformLocation(shader.id, "premultiplied");
            shader.glow = glGetUniformLocation(shader.id, "glow");
            shader.hslShift = glGetUniformLocation(shader.id, "hslShift");
            shader.matrixEnabled = glGetUniformLocation(shader.id, "matrixEnabled");
            shader.matrixRows = glGetUniformLocation(shader.id, "matrixRows[0]");
            shader.matrixBias = glGetUniformLocation(shader.id, "matrixBias");
        }
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, false, sizeof(Vertex), nullptr);
        glVertexAttribPointer(1, 2, GL_FLOAT, false, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));
        glVertexAttribPointer(2, 2, GL_FLOAT, false, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, clipZ)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glGenVertexArrays(1, &quadVao);
        glBindVertexArray(quadVao);
        glGenBuffers(1, &quadVbo);
        for (GLuint i = 0; i < 4; ++i) {
            glVertexAttribFormat(i, 3, GL_FLOAT, false, i * sizeof(Vertex));
            glVertexAttribFormat(i + 4, 4, GL_FLOAT, false, i * sizeof(Vertex) + offsetof(Vertex, u));
        }
        glVertexAttribFormat(8, 4, GL_FLOAT, false, offsetof(QuadInstance, multiply));
        glVertexAttribFormat(9, 4, GL_FLOAT, false, offsetof(QuadInstance, add));
        for (GLuint i = 0; i < 10; ++i) {
            glVertexAttribBinding(i, 0);
            glEnableVertexAttribArray(i);
        }
        glVertexBindingDivisor(0, 1);
        const uint32_t pixel = 0xffffffff;
        white = makeTexture(1, 1, &pixel);
        glGenSamplers(samplers.size(), samplers.data());
        for (size_t i = 0; i < samplers.size(); ++i) {
            const auto filter = i == 0 ? GL_LINEAR : GL_NEAREST;
            glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, filter);
            glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, filter);
            glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        canvas = makeTexture(Width, Height, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvas, 0);
        glGenRenderbuffers(1, &stencilBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, stencilBuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, Width, Height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencilBuffer);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("Could not create the cabinet framebuffer");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        sdvxRmlDrawTarget = framebuffer;

        FT_Init_FreeType(&freetype);
        for (size_t i = 0; i < fonts.size(); ++i) {
            const auto& spec = FontSpecs[i];
            auto& font = fonts[i];
            const auto previous = std::find_if(FontSpecs.begin(), FontSpecs.begin() + i,
                [&](const FontSpec& other) { return std::string_view(other.path) == spec.path; });
            if (previous == FontSpecs.begin() + i) font.bytes = readAsset(spec.path);
            const auto& bytes = fonts[previous - FontSpecs.begin()].bytes;
            if (!bytes.empty()) FT_New_Memory_Face(freetype, reinterpret_cast<const FT_Byte*>(bytes.data()),
                                                    bytes.size(), spec.faceIndex, &font.face);
        }

        Rml::String error;
        if (!RmlGL3::Initialize(&error)) throw std::runtime_error(error);
        uiRenderer = std::make_unique<UiRenderer>();
        if (!*uiRenderer) throw std::runtime_error("RmlUi GLES renderer did not initialize");
        uiRenderer->SetViewport(Width, Height);
        Rml::SetFileInterface(&files);
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(uiRenderer.get());
        if (!Rml::Initialise()) throw std::runtime_error("RmlUi did not initialize");
        for (size_t i = 0; i < fonts.size(); ++i) {
            const auto& spec = FontSpecs[i];
            const auto source = std::find_if(FontSpecs.begin(), FontSpecs.begin() + i + 1,
                [&](const FontSpec& other) { return std::string_view(other.path) == spec.path; });
            const auto& bytes = fonts[source - FontSpecs.begin()].bytes;
            if (!bytes.empty()) {
                Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(bytes.data()), bytes.size()},
                                  spec.family, Rml::Style::FontStyle::Normal,
                                  Rml::Style::FontWeight::Normal, spec.fallback, spec.faceIndex);
            }
        }
        context = Rml::CreateContext("cabinet", {int(Width), int(Height)});
    }

    ~Impl() {
        Rml::Shutdown();
        uiRenderer.reset();
        sdvxRmlOverlayTexture = 0;
        RmlGL3::Shutdown();
        for (auto& font : fonts) if (font.face) FT_Done_Face(font.face);
        if (freetype) FT_Done_FreeType(freetype);
        if (!pages.empty()) glDeleteTextures(pages.size(), pages.data());
        for (auto& [_, texture] : textures) if (texture.id) glDeleteTextures(1, &texture.id);
        glDeleteTextures(1, &white);
        glDeleteTextures(1, &canvas);
        glDeleteSamplers(samplers.size(), samplers.data());
        glDeleteProgram(stagePostProgram);
        glDeleteProgram(stageColorProgram);
        glDeleteFramebuffers(1, &stageColorTarget);
        glDeleteProgram(tunnelNoiseProgram);
        glDeleteFramebuffers(1, &tunnelTarget);
        glDeleteTextures(1, &tunnelImage);
        glDeleteTextures(1, &tunnelNoiseRows);
        glDeleteFramebuffers(1, &framebuffer);
        for (const auto& target : stageLayerTargets) {
            glDeleteFramebuffers(1, &target.framebuffer);
            glDeleteRenderbuffers(1, &target.stencil);
        }
        glDeleteFramebuffers(1, &canvasCopyTarget);
        glDeleteFramebuffers(1, &stageFilterTarget);
        glDeleteRenderbuffers(1, &stencilBuffer);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &quadVbo);
        glDeleteVertexArrays(1, &quadVao);
        glDeleteVertexArrays(1, &vao);
        for (const auto& shader : spritePrograms) glDeleteProgram(shader.id);
    }

    Texture& upload(std::string_view path, const Pixels& image) {
        auto& texture = textures.try_emplace(std::string(path)).first->second;
        if (image.data()) {
            texture = {makeTexture(image.w, image.h, image.data()), image.w, image.h, frame};
            textureBytes += size_t(image.w) * image.h * 4;
            nextTextureTrim = frame;
        }
        return texture;
    }

    Texture& finishImageDecode(std::vector<ImageLoad>::iterator pending) {
        auto& texture = upload(pending->path, pending->pixels.get());
        texture.stage |= pending->stage;
        decodingImages.erase(pending);
        return texture;
    }

    Texture& texture(std::string_view path) {
        auto found = textures.find(path);
        if (found == textures.end()) {
            const auto pending = std::ranges::find(decodingImages, path, &ImageLoad::path);
            if (pending != decodingImages.end()) return finishImageDecode(pending);
            return upload(path, decodeImage(path));
        }
        auto& result = found->second;
        result.used = frame;
        return result;
    }

    void prepareQueuedImage() {
        const bool stage = !stageImages.empty() || std::ranges::any_of(decodingImages, &ImageLoad::stage);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
        // A low frame rate during the transition should still drain a batch of
        // stage textures. Keep the per-frame upload time and decode backlog bounded.
        for (auto pending = decodingImages.begin(); pending != decodingImages.end();) {
            if (pending->pixels.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++pending;
                continue;
            }
            const auto index = pending - decodingImages.begin();
            finishImageDecode(pending);
            if (!stage || std::chrono::steady_clock::now() >= deadline) break;
            pending = decodingImages.begin() + index;
        }
        while (!stageImages.empty() || !queuedImages.empty()) {
            const bool stageImage = !stageImages.empty();
            if (decodingImages.size() >= (stageImage ? 8u : 1u)) break;
            auto& queue = stageImage ? stageImages : queuedImages;
            auto path = std::move(queue.front());
            queue.pop_front();
            if (auto found = textures.find(path); found != textures.end()) {
                found->second.stage |= stageImage;
            } else if (const auto pending = std::ranges::find(decodingImages, path, &ImageLoad::path);
                       pending != decodingImages.end()) {
                pending->stage |= stageImage;
            } else {
                auto pixels = std::async(std::launch::async, decodeImage, path);
                decodingImages.push_back({std::move(path), stageImage, std::move(pixels)});
            }
        }
    }

    void trimTextures() {
        constexpr size_t budget = 192 * 1024 * 1024;
        if (textureBytes <= budget || frame < nextTextureTrim) return;
        // Resident stage art can exceed the cache budget by itself. Retry
        // when a recent texture can age out, or when residency changes.
        nextTextureTrim = std::numeric_limits<uint64_t>::max();
        std::vector<std::pair<uint64_t, std::string_view>> old;
        for (const auto& [path, texture] : textures) {
            if (!texture.id || texture.stage || path.starts_with("runtime/")) continue;
            const auto expires = texture.used + 2;
            if (expires <= frame) old.emplace_back(texture.used, path);
            else nextTextureTrim = std::min(nextTextureTrim, expires);
        }
        std::sort(old.begin(), old.end());
        for (const auto& [_, path] : old) {
            auto it = textures.find(path);
            auto& texture = it->second;
            glDeleteTextures(1, &texture.id);
            textureBytes -= size_t(texture.w) * texture.h * 4;
            textures.erase(it);
            if (textureBytes <= budget) break;
        }
    }

    Rect clip() const { return clips.empty() ? Rect{0, 0, canvasW, canvasH} : clips.back(); }


    void scissor(Rect box) const {
        if (box == Rect{0, 0, canvasW, canvasH}) {
            glDisable(GL_SCISSOR_TEST);
            return;
        }
        glEnable(GL_SCISSOR_TEST);
        int x = std::floor(box.x), y = std::floor(canvasH - box.y - box.h);
        glScissor(x, y, std::ceil(box.x + box.w) - x, std::ceil(canvasH - box.y) - y);
    }

    void scissor() const { scissor(clip()); }

    void invalidateStencil() {
        stencilActive = false;
        stencilIndex = 0;
        stencilTarget = -1;
        stencilMasks.clear();
    }

    void draw(GLuint textureId, std::span<const Vertex> data, Color mul, Color extra, Blend mode,
              bool isPremultiplied = false, bool nearest = false, bool glow = false, std::array<float, 3> hsl = {},
              const ColorMatrix* filter = nullptr, bool stencil = false) {
        if (!textureId || data.empty()) return;
        if (playfieldMask) {
            if (playfieldMask->empty()) return;
            playfieldTriangles.clear();
            for (size_t index = 0; index < data.size(); index += 3) {
                playfieldPolygon.assign(data.begin() + index, data.begin() + index + 3);
                clipPolygon(playfieldPolygon, playfieldScratch, *playfieldMask, 1.f);
                for (size_t i = 2; i < playfieldPolygon.size(); ++i) {
                    playfieldTriangles.push_back(playfieldPolygon.front());
                    playfieldTriangles.push_back(playfieldPolygon[i - 1]);
                    playfieldTriangles.push_back(playfieldPolygon[i]);
                }
            }
            data = playfieldTriangles;
            if (data.empty()) return;
        }
        if (stencilActive && !stencil) flush();
        const bool instanced = data.size() == 6 && data[0] == data[3] && data[2] == data[4];
        Material material{textureId, instanced ? White : mul, instanced ? Transparent : extra,
                          mode, isPremultiplied, nearest, glow, hsl, {}};
        if (filter) material.filter = *filter;
        const Rect box = clip();
        const auto count = instanced ? 1 : static_cast<GLsizei>(data.size());
        if (!draws.empty() && draws.back().instanced == instanced && draws.back().clip == box &&
            draws.back().material == material) draws.back().count += count;
        else draws.push_back({material, box, static_cast<GLint>(instanced ? quads.size() : vertices.size()), count, instanced});
        const auto transformed = [&](Vertex v) {
            if (!transforms.empty()) {
                const auto& m = transforms.back();
                const auto x = m[0]*v.x + m[1]*v.y + m[2]*v.w;
                v.y = m[3]*v.x + m[4]*v.y + m[5]*v.w;
                v.x = x;
            }
            return v;
        };
        if (instanced) {
            // Preserve the original two triangles, including perspective, UVs,
            // depth and per-corner alpha. Colors no longer split quad batches.
            quads.push_back({{transformed(data[0]), transformed(data[1]), transformed(data[2]), transformed(data[5])}, mul, extra});
        } else {
            for (const auto& v : data) vertices.push_back(transformed(v));
        }
    }

    void flush() {
        if (draws.empty()) scissor();
        else flushBatch();
        if (stencilActive) {
            glDisable(GL_STENCIL_TEST);
            stencilActive = false;
        }
    }

    // Each draw keeps its scissor. Stencil writes and external renderers still
    // flush, since they depend on the current contents of the framebuffer.
    void flushBatch() {
        if (draws.empty()) return;
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, drawingLaserGlow ? GL_MAX : GL_FUNC_ADD);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        // Each flushed batch gets fresh storage. Appending with BufferSubData
        // makes Adreno copy the in-flight buffer for every AFP stencil flush.
        if (!vertices.empty())
            glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STREAM_DRAW);
        // Cubism uses client arrays on VAO 0. Reassert the buffered attributes
        // after that handoff: the Android GLES guest otherwise drops the first
        // batch on subsequent frames, despite reporting the saved VAO correctly.
        glVertexAttribPointer(0, 3, GL_FLOAT, false, sizeof(Vertex), nullptr);
        glVertexAttribPointer(1, 2, GL_FLOAT, false, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));
        glVertexAttribPointer(2, 2, GL_FLOAT, false, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, clipZ)));
        if (!quads.empty()) {
            glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
            glBufferData(GL_ARRAY_BUFFER, quads.size() * sizeof(QuadInstance), quads.data(), GL_STREAM_DRAW);
        }
        const Material* previous = nullptr;
        GLuint activeProgram = 0;
        Rect previousClip{};
        glBlendEquationSeparate(GL_FUNC_ADD, stageLayerDepth ? GL_MAX : GL_FUNC_ADD);
        for (const auto& command : draws) {
            const auto& material = command.material;
            const auto& shader = spritePrograms[(material.filter.has_value() || material.hsl != std::array<float, 3>{}) + (command.instanced ? 2 : 0)];
            if (activeProgram != shader.id) {
                glUseProgram(shader.id);
                glUniform2f(shader.canvasSize, canvasW, canvasH);
                activeProgram = shader.id;
                previous = nullptr;
            }
            if (!previous || previousClip != command.clip) {
                scissor(command.clip);
                previousClip = command.clip;
            }
            if (!previous || previous->blend != material.blend) {
                switch (material.blend) {
                    case Blend::Alpha: glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                                           stageLayerDepth ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA); break;
                    case Blend::Add: glBlendFunc(GL_ONE, GL_ONE); break;
                    case Blend::Multiply:
                        glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                            stageLayerDepth ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                        break;
                }
            }
            if (!previous || previous->multiply != material.multiply) {
                const auto& c = material.multiply;
                glUniform4f(shader.multiply, c.r, c.g, c.b, c.a);
            }
            if (!previous || previous->add != material.add) {
                const auto& c = material.add;
                glUniform4f(shader.add, c.r, c.g, c.b, c.a);
            }
            if (!previous || previous->premultiplied != material.premultiplied)
                glUniform1i(shader.premultiplied, material.premultiplied);
            if (!previous || previous->glow != material.glow)
                glUniform1i(shader.glow, material.glow);
            if (!previous || previous->hsl != material.hsl)
                glUniform3fv(shader.hslShift, 1, material.hsl.data());
            if (!previous || previous->filter != material.filter) {
                glUniform1i(shader.matrixEnabled, material.filter.has_value());
                if (material.filter) {
                    std::array<float, 16> rows;
                    std::array<float, 4> bias;
                    for (size_t row = 0; row < 4; ++row) {
                        std::copy_n(material.filter->begin() + row * 5, 4, rows.begin() + row * 4);
                        bias[row] = (*material.filter)[row * 5 + 4];
                    }
                    glUniform4fv(shader.matrixRows, 4, rows.data());
                    glUniform4fv(shader.matrixBias, 1, bias.data());
                }
            }
            if (!previous || previous->texture != material.texture)
                glBindTexture(GL_TEXTURE_2D, material.texture);
            if (!previous || previous->nearest != material.nearest)
                glBindSampler(0, samplers[material.nearest]);
            if (command.instanced) {
                glBindVertexArray(quadVao);
                glBindVertexBuffer(0, quadVbo, command.first * sizeof(QuadInstance), sizeof(QuadInstance));
                glDrawArraysInstanced(GL_TRIANGLES, 0, 6, command.count);
            } else {
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLES, command.first, command.count);
            }
            previous = &material;
        }
        if (previousClip != clip()) scissor();
        vertices.clear();
        quads.clear();
        draws.clear();
        glBindSampler(0, 0);
    }

    void texturedRect(GLuint textureId, Rect bounds, Rect uv, Color color, Blend blend,
                      bool isPremultiplied = false) {
        float x1 = bounds.x, x2 = x1 + bounds.w, y1 = bounds.y, y2 = y1 + bounds.h;
        float u1 = uv.x, u2 = u1 + uv.w, v1 = uv.y, v2 = v1 + uv.h;
        const Vertex a{x1,y1,1,u1,v1}, b{x2,y1,1,u2,v1}, c{x2,y2,1,u2,v2}, d{x1,y2,1,u1,v2};
        const std::array data{a,b,c,a,c,d};
        draw(textureId, data, color, Transparent, blend, isPremultiplied);
    }

    Glyph glyph(int face, int size, uint32_t code, int outlineWidth = 0) {
        uint64_t key = (uint64_t(outlineWidth) << 56) | (uint64_t(face) << 48) | (uint64_t(size) << 32) | code;
        auto found = glyphs.find(key);
        if (found != glyphs.end()) return found->second;
        FT_Face font = fonts[face].face;
        constexpr int body = static_cast<int>(TextFont::Body);
        constexpr int korean = static_cast<int>(TextFont::Korean);
        if (!font || !FT_Get_Char_Index(font, code)) {
            if (face != body && face != korean) return glyph(body, size, code, outlineWidth);
            if (face == body && fonts[korean].face && FT_Get_Char_Index(fonts[korean].face, code))
                return glyph(korean, size, code, outlineWidth);
        }
        if (!font) return {};
        FT_Set_Pixel_Sizes(font, 0, size);
        if (FT_Load_Char(font, code, outlineWidth > 0 ? FT_LOAD_NO_BITMAP : FT_LOAD_RENDER)) return {};
        auto* slot = font->glyph;
        const FT_Bitmap* bitmap = &slot->bitmap;
        Glyph result{0, {}, slot->advance.x / 64.0f, float(slot->bitmap_left), float(slot->bitmap_top), 0, 0};
        FT_Glyph outlined = nullptr;
        if (outlineWidth > 0) {
            FT_Stroker stroker;
            if (FT_Stroker_New(freetype, &stroker)) return {};
            FT_Stroker_Set(stroker, outlineWidth * 64, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
            const bool failed = FT_Get_Glyph(slot, &outlined) ||
                FT_Glyph_StrokeBorder(&outlined, stroker, false, true) ||
                FT_Glyph_To_Bitmap(&outlined, FT_RENDER_MODE_NORMAL, nullptr, true);
            FT_Stroker_Done(stroker);
            if (failed) {
                if (outlined) FT_Done_Glyph(outlined);
                return {};
            }
            const auto* stroked = reinterpret_cast<FT_BitmapGlyph>(outlined);
            bitmap = &stroked->bitmap;
            result.left = stroked->left;
            result.top = stroked->top;
        }
        const int w = bitmap->width, h = bitmap->rows;
        result.w = w;
        result.h = h;
        if (w && h) {
            if (glyphX + w + 2 > AtlasSize) {
                glyphX = 0;
                glyphY += rowHeight;
                rowHeight = 0;
            }
            if (pages.empty() || glyphY + h + 2 > AtlasSize) {
                pages.push_back(makeTexture(AtlasSize, AtlasSize, nullptr));
                glyphX = glyphY = rowHeight = 0;
            }
            std::vector<uint8_t> pixels(size_t(w + 2) * (h + 2) * 4, 0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    size_t offset = (size_t(y + 1) * (w + 2) + x + 1) * 4;
                    pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = 255;
                    pixels[offset + 3] = bitmap->buffer[y * bitmap->pitch + x];
                }
            }
            result.page = pages.back();
            glBindTexture(GL_TEXTURE_2D, result.page);
            glTexSubImage2D(GL_TEXTURE_2D, 0, glyphX, glyphY, w + 2, h + 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            result.uv = {float(glyphX + 1) / AtlasSize, float(glyphY + 1) / AtlasSize,
                         float(w) / AtlasSize, float(h) / AtlasSize};
            glyphX += w + 2;
            rowHeight = std::max(rowHeight, h + 2);
        }
        if (outlined) FT_Done_Glyph(outlined);
        glyphs.emplace(key, result);
        return result;
    }
};

Render::Render(SDL_Window* window) : impl_(std::make_unique<Impl>(window)) {}
Render::~Render() = default;
void Render::resetGraphics() {
    const auto window = impl_->window;
    const bool landscape = impl_->canvasW > impl_->canvasH;
    auto afp = std::move(impl_->afp);
    // Destroy every stale GL name before allocating in the replacement context.
    // RmlUi geometry, glyph atlases and captured textures belong to that context too.
    impl_.reset();
    // The lost context's shader/program names are invalid in the new context.
    // Discard only cleanup errors here, before creating any replacement objects.
    while (glGetError() != GL_NO_ERROR) {}
    impl_ = std::make_unique<Impl>(window);
    impl_->afp = std::move(afp);
    setLandscape(landscape);
}
void Render::flush() { impl_->flush(); }

void Render::setLandscape(bool landscape) {
    auto& p = *impl_;
    const float width = landscape ? Height : Width, height = landscape ? Width : Height;
    if (p.canvasW == width) return;
    p.flush();
    p.canvasW = width;
    p.canvasH = height;
    glBindTexture(GL_TEXTURE_2D, p.canvas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindRenderbuffer(GL_RENDERBUFFER, p.stencilBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    p.uiRenderer->SetViewport(width, height);
    p.context->SetDimensions({int(width), int(height)});
    p.uiResized = true;
}

bool Render::displayReady(bool landscape) const {
    int width, height;
    SDL_GetWindowSizeInPixels(impl_->window, &width, &height);
    return width > 0 && height > 0 && (width > height) == landscape;
}

void Render::captureImage(std::string_view name, int width, int height, const std::function<void()>& draw) {
    auto& p = *impl_;
    p.flush();
    auto entry = p.textures.try_emplace(std::string(name)).first;
    // A size lookup before capture may already have cached a missing runtime image.
    if (!entry->second.id) {
        entry->second = {makeTexture(width, height, nullptr), width, height, p.frame, true};
        p.textureBytes += size_t(width) * height * 4;
        p.nextTextureTrim = p.frame;
    }
    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    GLuint target, stencil;
    glGenFramebuffers(1, &target);
    glGenRenderbuffers(1, &stencil);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, entry->second.id, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
    const auto canvasW = p.canvasW, canvasH = p.canvasH;
    p.canvasW = width; p.canvasH = height;
    p.clips.clear(); p.invalidateStencil();
    glViewport(0,0,width,height);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glStencilMask(0xff); glClearStencil(0);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    draw();
    p.flush();
    p.canvasW = canvasW; p.canvasH = canvasH;
    glBindFramebuffer(GL_FRAMEBUFFER, previous);
    glViewport(0,0,canvasW,canvasH);
    glDeleteRenderbuffers(1,&stencil);
    glDeleteFramebuffers(1,&target);
}

void Render::prepareCanvasCopy(std::string_view name) {
    auto& p = *impl_;
    const int width = static_cast<int>(p.canvasW), height = static_cast<int>(p.canvasH);
    auto [entry, created] = p.textures.try_emplace(std::string(name));
    if (created) {
        entry->second = {makeTexture(width, height, nullptr), width, height, p.frame, true};
        p.textureBytes += size_t(width) * height * 4;
        p.nextTextureTrim = p.frame;
    }
}

void Render::copyCanvas(std::string_view name) {
    auto& p = *impl_;
    p.flush();
    prepareCanvasCopy(name);
    const auto& texture = p.textures.find(name)->second;
    GLint previous;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
    if (!p.canvasCopyTarget) glGenFramebuffers(1, &p.canvasCopyTarget);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p.canvasCopyTarget);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.id, 0);
    // Write into the existing image even when AFP has not sampled the previous
    // capture. CopyTexSubImage2D can accumulate deferred staging images in ANGLE.
    // A canvas copy ignores the caller's clip, just like CopyTexSubImage2D.
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, texture.w, texture.h, 0, 0, texture.w, texture.h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
    p.scissor();
}

void Render::prepareStageLayer() {
    auto& p = *impl_;
    if (p.stageLayerDepth < p.stageLayerTargets.size()) return;
    auto& target = p.stageLayerTargets.emplace_back();
    glGenFramebuffers(1, &target.framebuffer);
    glGenRenderbuffers(1, &target.stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, target.stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, Width, Height);
    GLint previous;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.framebuffer);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.stencil);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
}

void Render::captureStageLayer(std::string_view name, const std::function<void()>& draw) {
    auto& p = *impl_;
    p.flush();
    prepareCanvasCopy(name);
    prepareStageLayer();
    GLint previousDraw, previousRead;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glBindFramebuffer(GL_FRAMEBUFFER, p.stageLayerTargets[p.stageLayerDepth++].framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.textures.find(name)->second.id, 0);
    constexpr GLfloat clear[]{0, 0, 0, 0};
    constexpr GLint clearStencil[]{0};
    const auto stencilIndex = p.stencilIndex;
    p.invalidateStencil();
    glDisable(GL_SCISSOR_TEST);
    glStencilMask(0xff);
    glClearBufferfv(GL_COLOR, 0, clear);
    glClearBufferiv(GL_STENCIL, 0, clearStencil);
    p.scissor();
    draw();
    p.flush();
    --p.stageLayerDepth;
    glBlendEquationSeparate(GL_FUNC_ADD, p.stageLayerDepth ? GL_MAX : GL_FUNC_ADD);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDraw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
    p.invalidateStencil();
    p.stencilIndex = stencilIndex;
}

void Render::filterStageLayer(std::string_view source, std::string_view destination,
                              const std::function<void()>& effect) {
    auto& p = *impl_;
    p.flush();
    prepareCanvasCopy(destination);
    const auto& output = p.textures.find(destination)->second;
    GLint previousDraw, previousRead;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    if (!p.stageFilterTarget) glGenFramebuffers(1, &p.stageFilterTarget);
    glBindFramebuffer(GL_FRAMEBUFFER, p.stageFilterTarget);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output.id, 0);
    // Each filter overwrites a full stage image; it needs neither a cleared
    // intermediate canvas nor a stencil attachment.
    glDisable(GL_SCISSOR_TEST);
    const auto previousInput = std::exchange(p.stagePostInput, p.textures.find(source)->second.id);
    effect();
    if (p.stagePostInput) {
        if (!p.canvasCopyTarget) glGenFramebuffers(1, &p.canvasCopyTarget);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, p.canvasCopyTarget);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.stagePostInput, 0);
        glBlitFramebuffer(0, 0, output.w, output.h, 0, 0, output.w, output.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    p.stagePostInput = previousInput;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDraw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
    p.scissor();
}

void Render::prepareTunnelNoise() {
    auto& p = *impl_;
    if (p.tunnelTarget) return;
    constexpr auto vertex = R"(
        out vec2 uv;
        void main() {
            uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
            gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
        }
    )";
    // 18092c440 samples a 1920x1 row texture and wraps the displaced X.
    constexpr auto fragment = R"(
        precision highp float;
        in vec2 uv;
        uniform sampler2D tunnel;
        uniform sampler2D rows;
        uniform int seed;
        out vec4 color;
        void main() {
            vec2 p = vec2(uv.x, 1.0 - uv.y);
            float random = fract(sin(dot(p, vec2(12.9898, 78.233)) + float(seed)) * 43758.5453);
            vec3 row = texture(rows, vec2(p.y, 0.5)).rgb;
            float jitter = (row.g + row.b * 0.01) * (random <= 0.5 ? random : -random);
            color = texture(tunnel, vec2(fract(p.x - row.r + jitter), uv.y));
        }
    )";
    const auto vs = compileShader(GL_VERTEX_SHADER, vertex);
    const auto fs = compileShader(GL_FRAGMENT_SHADER, fragment);
    p.tunnelNoiseProgram = glCreateProgram();
    glAttachShader(p.tunnelNoiseProgram, vs);
    glAttachShader(p.tunnelNoiseProgram, fs);
    glLinkProgram(p.tunnelNoiseProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked;
    glGetProgramiv(p.tunnelNoiseProgram, GL_LINK_STATUS, &linked);
    if (!linked) throw std::runtime_error("Could not link SIDEBG noise effect");
    glUseProgram(p.tunnelNoiseProgram);
    glUniform1i(glGetUniformLocation(p.tunnelNoiseProgram, "tunnel"), 0);
    glUniform1i(glGetUniformLocation(p.tunnelNoiseProgram, "rows"), 1);
    p.tunnelNoiseSeed = glGetUniformLocation(p.tunnelNoiseProgram, "seed");

    p.tunnelImage = makeTexture(Width, Height, nullptr);
    p.tunnelNoiseRows = makeTexture(Height, 1, nullptr);
    GLint previous;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
    glGenFramebuffers(1, &p.tunnelTarget);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p.tunnelTarget);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.tunnelImage, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
}

void Render::drawTunnelNoise(std::span<const std::byte> rows, int seed, const std::function<void()>& draw) {
    auto& p = *impl_;
    p.flush();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p.tunnelTarget);
    glDisable(GL_SCISSOR_TEST);
    const GLfloat transparent[]{0, 0, 0, 0};
    glClearBufferfv(GL_COLOR, 0, transparent);
    draw();
    p.flush();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p.framebuffer);
    p.scissor();

    glUseProgram(p.tunnelNoiseProgram);
    glUniform1i(p.tunnelNoiseSeed, seed);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p.tunnelNoiseRows);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Height, 1, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    glBindSampler(1, p.samplers[1]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p.tunnelImage);
    glBindSampler(0, p.samplers[0]);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(p.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(1, 0);
    glBindSampler(0, 0);
}

void Render::stagePostEffect(float chromatic, float radial, float distortion, bool crt) {
    stagePostEffect(chromatic, radial, distortion, crt, {.5f, .75f}, 0, 0, 0);
}

unsigned int Render::stagePostSource() {
    constexpr auto copy = "runtime/stage-post-effect";
    auto& p = *impl_;
    // Captured filter chains already have a separate, immutable input image.
    if (const auto input = std::exchange(p.stagePostInput, 0)) {
        p.flush();
        return input;
    }
    GLint target;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    if (target == static_cast<GLint>(p.framebuffer) && p.clip() == Rect{0, 0, p.canvasW, p.canvasH}) {
        p.flush();
        prepareCanvasCopy(copy);
        // These passes overwrite the whole canvas. Keep its previous color as
        // input and render into the scratch texture instead of copying 8 MB.
        // The framebuffer and its stencil stay in place for AFP and Cubism.
        std::swap(p.canvas, p.textures.find(copy)->second.id);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p.canvas, 0);
        constexpr GLenum discard[]{GL_COLOR_ATTACHMENT0};
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, discard);
    } else {
        // Captured layers keep their named texture; clipped passes must also
        // retain the untouched pixels outside the clip.
        copyCanvas(copy);
    }
    return p.textures.find(copy)->second.id;
}

void Render::stageNoise(std::span<const std::byte> rows, int seed) {
    auto& p = *impl_;
    const auto source = stagePostSource();
    prepareTunnelNoise();
    glDisable(GL_BLEND);
    glUseProgram(p.tunnelNoiseProgram);
    glUniform1i(p.tunnelNoiseSeed, seed);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p.tunnelNoiseRows);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Height, 1, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    glBindSampler(1, p.samplers[1]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindSampler(0, p.samplers[0]);
    glBindVertexArray(p.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(1, 0);
    glBindSampler(0, 0);
}

void Render::stageLuminous(float strength, float renderMax, float renderAdd) {
    constexpr auto source = "runtime/stage-post-effect";
    copyCanvas(source);
    prepareLaserGlow();
    auto& p = *impl_;
    GLint target;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
    p.laserGlow->draw(target, p.textures.find(source)->second.id, strength, renderMax, renderAdd);
    p.scissor();
}

void Render::stagePostEffect(float chromatic, float radial, float distortion, bool crt,
                             Vec2 center, float seconds, float scanline, float darkEdge) {
    stagePostEffect(chromatic, radial, distortion, crt, center, seconds, scanline, darkEdge, {});
}

void Render::stagePostEffect(float chromatic, float radial, float distortion, bool crt,
                             Vec2 center, float seconds, float scanline, float darkEdge, Vec2 offset) {
    auto& p = *impl_;
    const auto source = stagePostSource();
    prepareStagePostEffect();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(p.stagePostProgram);
    glUniform4f(p.postEffect, chromatic, radial, distortion, crt ? 1.f : 0.f);
    glUniform2f(p.postCenter, center.x, center.y);
    glUniform2f(p.postOffset, offset.x, offset.y);
    glUniform3f(p.postCrt, seconds, scanline, darkEdge);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindSampler(0, p.samplers[0]);
    glBindVertexArray(p.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
}

void Render::prepareStagePostEffect() {
    auto& p = *impl_;
    if (!p.stagePostProgram) {
        constexpr auto vertex = R"(
            precision highp float;
            out vec2 uv;
            uniform vec3 crtAnimation;
            flat out vec4 vhs;
            flat out vec2 vhsOffset;
            float random(vec2 point) {
                return fract(sin(dot(point, vec2(12.9898, 78.233))) * 43758.5453);
            }
            void main() {
                uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
                gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
                // The phase and drift are constant across the full-screen pass.
                float time = crtAnimation.x;
                float phase = floor(time);
                float length = 0.2 + 1.3 * random(vec2(phase, 1));
                float progress = fract(time);
                float held = clamp(1.0 - progress / max(length, 0.2), 0.0, 1.0);
                float signal = held * random(vec2(phase, 2)) +
                    clamp(0.04 * random(vec2(phase, 3)) - 0.02, 0.0, 1.0);
                float drift = progress * signal * 4.0 + signal;
                float waviness = (random(vec2(phase, 4)) * 0.04 - 0.02) * 0.05;
                vhs = vec4(signal, drift, waviness, min(30.0 * random(vec2(phase, 7)), 10.0));
                vhsOffset = vec2(2.0 * random(vec2(phase, 5)) - 1.0,
                                 0.2 * random(vec2(phase, 6)) - 0.1) * signal;
            }
        )";
        // Color offsets, six radial taps and CRT curvature from the original pixel shaders.
        constexpr auto fragment = R"(
            precision highp float;
            in vec2 uv;
            flat in vec4 vhs;
            flat in vec2 vhsOffset;
            uniform sampler2D scene;
            uniform vec4 effect;
            uniform vec2 blurCenter;
            uniform vec2 shakeOffset;
            uniform vec3 crtAnimation;
            out vec4 color;
            float random(vec2 point) {
                return fract(sin(dot(point, vec2(12.9898, 78.233))) * 43758.5453);
            }
            vec4 separated(vec2 p) {
                vec2 delta = p - 0.5;
                vec4 raw = texture(scene, p);
                if (effect.x != 0.0) {
                    raw.r = texture(scene, p - delta * 2.0 * effect.x).r;
                    raw.g = texture(scene, p - delta * effect.x).g;
                }
                return raw;
            }
            void main() {
                vec2 p = uv - shakeOffset;
                if (any(lessThan(p, vec2(0.0))) || any(greaterThan(p, vec2(1.0)))) {
                    color = vec4(0.0);
                    return;
                }
                float vignet = length(p - 0.5);
                if (effect.w != 0.0) {
                    vec2 centered = p - 0.5;
                    centered /= 1.0 - vignet * effect.z;
                    if (max(abs(centered.x), abs(centered.y)) > 0.5) {
                        color = vec4(0.0, 0.0, 0.0, 1.0);
                        return;
                    }
                    p = centered + 0.5;
                }
                vec2 center = blurCenter;
                vec2 delta = p - center;
                vec4 outputColor = separated(p);
                if (effect.y != 0.0) {
                    float distanceToCenter = length(delta);
                    for (int j = 1; j < 6; ++j)
                        outputColor += separated(center + delta * (1.0 - effect.y * float(j) / 6.0 * distanceToCenter));
                    outputColor /= 6.0;
                }
                if (effect.w != 0.0) {
                    // 180393070 / 180393760 choose short-lived random VHS offsets.
                    // Keep the same duration and uniform ranges on the chart clock.
                    float time = crtAnimation.x;
                    float signal = vhs.x;
                    float waveFrequency = 100.0;
                    vec2 samplePosition = vec2(p.x, 1.0 - p.y);
                    samplePosition.x += sin(samplePosition.y * waveFrequency + vhs.y * 20.0) * vhs.z;
                    samplePosition += vhsOffset;
                    float rowNoise = random(vec2(floor(samplePosition.y * 500.0) + time)) - 0.5;
                    samplePosition.x += rowNoise * clamp(signal * 0.5, 0.0, 1.0);
                    samplePosition = fract(samplePosition);
                    vec2 sampleUv = vec2(samplePosition.x, 1.0 - samplePosition.y);
                    vec3 rgb = vec3(texture(scene, sampleUv).r,
                                    texture(scene, sampleUv - vec2(0.002, 0.0)).g,
                                    texture(scene, sampleUv - vec2(0.004, 0.0)).b);
                    float alpha = texture(scene, sampleUv - vec2(0.003, 0.0)).a;
                    if (random(vec2(rowNoise + time)) < clamp(signal * 0.7, 0.0, 1.0)) {
                        vec2 centered = vec2(p.x - 0.5, 0.5 - p.y) + vec2(123.0 + time, 0.0);
                        rgb = vec3(random(centered), random(centered + vec2(0.0, 1.0)),
                                   random(centered + vec2(0.0, 2.0)));
                        alpha = 1.0;
                    }
                    float stripe = fract(gl_FragCoord.x / 3.0);
                    rgb *= vec3(stripe > 0.3333 ? 1.0 : 0.0,
                                stripe < 0.3333 || stripe > 0.6666 ? 1.0 : 0.0,
                                stripe < 0.6666 ? 1.0 : 0.0);
                    float scan = sin(time * 10.0 + (0.5 - p.y) * 500.0) * 0.5 + 0.5;
                    rgb *= mix(1.0, 0.5 + clamp(scan + 0.5, 0.0, 1.0) * 0.5, crtAnimation.y);
                    float tailLength = vhs.w;
                    float tail = clamp((fract(0.5 - p.y + time) - 1.0 + tailLength) / clamp(tailLength, 0.0001, 1.0), 0.0, 1.0);
                    rgb *= mix(1.0, tail, crtAnimation.y);
                    rgb *= mix(1.0, 1.0 - vignet * 1.3, crtAnimation.z);
                    outputColor = vec4(rgb, alpha);
                }
                color = outputColor;
            }
        )";
        const auto vs = compileShader(GL_VERTEX_SHADER, vertex), fs = compileShader(GL_FRAGMENT_SHADER, fragment);
        p.stagePostProgram = glCreateProgram();
        glAttachShader(p.stagePostProgram, vs);
        glAttachShader(p.stagePostProgram, fs);
        glLinkProgram(p.stagePostProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked;
        glGetProgramiv(p.stagePostProgram, GL_LINK_STATUS, &linked);
        if (!linked) throw std::runtime_error("Could not link stage post effect");
        p.postEffect = glGetUniformLocation(p.stagePostProgram, "effect");
        p.postCenter = glGetUniformLocation(p.stagePostProgram, "blurCenter");
        p.postOffset = glGetUniformLocation(p.stagePostProgram, "shakeOffset");
        p.postCrt = glGetUniformLocation(p.stagePostProgram, "crtAnimation");
    }
}

void Render::stageColorConversion(int mode, float strength) {
    drawStageColorConversion(stagePostSource(), mode, strength);
}

void Render::prepareStageColorConversion() {
    auto& p = *impl_;
    if (!p.stageColorProgram) {
        const auto vertex = compileShader(GL_VERTEX_SHADER, R"(
            out vec2 uv;
            void main() {
                uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
                gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
            }
        )");
        // Modes 0–4 use the original ColorConversion shader table at 180391e30.
        const auto fragment = compileShader(GL_FRAGMENT_SHADER, R"(
            precision highp float;
            in vec2 uv;
            uniform sampler2D scene;
            uniform int mode;
            uniform float strength;
            out vec4 color;
            void main() {
                vec4 raw = texture(scene, uv);
                vec3 rgb = raw.a > 0.0 ? raw.rgb / raw.a : vec3(0.0);
                float low = min(rgb.r, min(rgb.g, rgb.b));
                float high = max(rgb.r, max(rgb.g, rgb.b));
                vec3 converted;
                if (mode == 4) {
                    float chroma = high - low;
                    float hue = 0.0;
                    if (chroma >= 1.0 / 256.0) {
                        if (high == rgb.r) hue = (rgb.g - rgb.b) / chroma;
                        else if (high == rgb.g) hue = 2.0 + (rgb.b - rgb.r) / chroma;
                        else hue = 4.0 + (rgb.r - rgb.g) / chroma;
                        vec3 sector = mod(hue + strength / 60.0 + vec3(2.0, 0.0, -2.0), 6.0);
                        converted = low + chroma * clamp(min(sector, 4.0 - sector), 0.0, 1.0);
                    } else {
                        converted = vec3((high + low) * 0.5);
                    }
                } else {
                    if (mode == 0) converted = vec3(dot(rgb, vec3(0.298912, 0.586611, 0.114478)));
                    else if (mode == 1) converted = vec3(dot(rgb, vec3(0.393, 0.769, 0.189)),
                                                         dot(rgb, vec3(0.349, 0.686, 0.168)),
                                                         dot(rgb, vec3(0.272, 0.534, 0.131)));
                    else if (mode == 2) converted = 1.0 - rgb;
                    else converted = rgb + (1.0 - low - high);
                    converted = mix(rgb, converted, strength * 0.01);
                }
                color = vec4(clamp(converted, 0.0, 1.0) * raw.a, raw.a);
            }
        )");
        p.stageColorProgram = glCreateProgram();
        glAttachShader(p.stageColorProgram, vertex);
        glAttachShader(p.stageColorProgram, fragment);
        glLinkProgram(p.stageColorProgram);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked;
        glGetProgramiv(p.stageColorProgram, GL_LINK_STATUS, &linked);
        if (!linked) throw std::runtime_error("Could not link stage color conversion");
        p.stageColorMode = glGetUniformLocation(p.stageColorProgram, "mode");
        p.stageColorStrength = glGetUniformLocation(p.stageColorProgram, "strength");
        glGenFramebuffers(1, &p.stageColorTarget);
    }
}

void Render::convertStageColor(std::string_view source, std::string_view destination, int mode, float strength) {
    auto& p = *impl_;
    p.flush();
    GLint previous = 0;
    if (!destination.empty()) {
        prepareStageColorConversion();
        prepareCanvasCopy(destination);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p.stageColorTarget);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               p.textures.find(destination)->second.id, 0);
        glDisable(GL_SCISSOR_TEST);
    }
    drawStageColorConversion(p.textures.find(source)->second.id, mode, strength);
    if (!destination.empty()) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
        p.scissor();
    }
}

void Render::drawStageColorConversion(unsigned int texture, int mode, float strength) {
    auto& p = *impl_;
    prepareStageColorConversion();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(p.stageColorProgram);
    glUniform1i(p.stageColorMode, mode);
    glUniform1f(p.stageColorStrength, strength);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, p.samplers[1]);
    glBindVertexArray(p.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindSampler(0, 0);
}

void Render::begin() {
    auto& p = *impl_;
    SDL_GetWindowSizeInPixels(p.window, &p.windowW, &p.windowH);
    float scale = std::min(p.windowW / p.canvasW, p.windowH / p.canvasH);
    p.viewW = std::lround(scale * p.canvasW);
    p.viewH = std::lround(scale * p.canvasH);
    p.viewX = (p.windowW - p.viewW) / 2;
    p.viewY = (p.windowH - p.viewH) / 2;
    ++p.frame;
    p.clips.clear();
    p.transforms.clear();
    p.invalidateStencil();
    glBindFramebuffer(GL_FRAMEBUFFER, p.framebuffer);
    glViewport(0, 0, p.canvasW, p.canvasH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glClearStencil(0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Render::pushTransform(const std::array<float, 6>& m) {
    auto& stack = impl_->transforms;
    if (stack.empty()) stack.push_back(m);
    else {
        const auto p = stack.back();
        stack.push_back({p[0]*m[0]+p[1]*m[3], p[0]*m[1]+p[1]*m[4], p[0]*m[2]+p[1]*m[5]+p[2],
                         p[3]*m[0]+p[4]*m[3], p[3]*m[1]+p[4]*m[4], p[3]*m[2]+p[4]*m[5]+p[5]});
    }
}

void Render::popTransform() { impl_->transforms.pop_back(); }

void Render::end() {
    auto& p = *impl_;
    p.flush();
    // Presentation samples only the canvas color. Avoid a depth/stencil store
    // on tile GPUs before the next frame clears those attachments.
    constexpr GLenum discard[]{GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 2, discard);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, p.framebuffer);
    glDisable(GL_SCISSOR_TEST);
    if (p.viewW != p.windowW || p.viewH != p.windowH) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBlitFramebuffer(0, 0, p.canvasW, p.canvasH, p.viewX, p.viewY, p.viewX + p.viewW,
                      p.viewY + p.viewH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    SDL_GL_SwapWindow(p.window);
    p.prepareQueuedImage();
    p.trimTextures();
}

Vec2 Render::logical(float x, float y) const {
    auto& p = *impl_;
    return {(x * p.windowW - p.viewX) * p.canvasW / std::max(1, p.viewW),
            (y * p.windowH - p.viewY) * p.canvasH / std::max(1, p.viewH)};
}

void Render::rect(Rect bounds, Color color) {
    impl_->texturedRect(impl_->white, bounds, {0, 0, 1, 1}, color, Blend::Alpha);
}

void Render::polygon(std::span<const Vec2> vertices, Color color, Blend blend) {
    impl_->draw(impl_->white, triangles(vertices), color, Transparent, blend);
}

void Render::line(Vec2 a, Vec2 b, float width, Color color) {
    float dx = b.x - a.x, dy = b.y - a.y, distance = std::hypot(dx, dy);
    if (distance == 0) return;
    Vec2 normal{-dy * width / (2 * distance), dx * width / (2 * distance)};
    const std::array points{Vec2{a.x + normal.x, a.y + normal.y}, Vec2{b.x + normal.x, b.y + normal.y},
                            Vec2{b.x - normal.x, b.y - normal.y}, Vec2{a.x - normal.x, a.y - normal.y}};
    polygon(points, color);
}

Vec2 Render::imageSize(std::string_view path) {
    auto& t = impl_->texture(path);
    return {float(t.w), float(t.h)};
}

void Render::queueImage(std::string_view path, bool stage) {
    auto& p = *impl_;
    if (auto found = p.textures.find(path); found != p.textures.end()) {
        found->second.used = p.frame;
        found->second.stage |= stage;
    } else if (const auto pending = std::ranges::find(p.decodingImages, path, &Impl::ImageLoad::path);
               pending != p.decodingImages.end()) {
        pending->stage |= stage;
    } else {
        auto& queue = stage ? p.stageImages : p.queuedImages;
        if (std::ranges::find(queue, path) == queue.end()) queue.emplace_back(path);
    }
}

void Render::image(std::string_view path, Rect bounds, Color color, Blend blend) {
    const auto texture = impl_->texture(path);
    impl_->texturedRect(texture.id, bounds, {0, 0, 1, 1}, color, blend, texture.premultiplied);
}

void Render::quad(std::string_view path, const std::array<Vec2, 4>& corners, Color mul, Color add, Blend blend, Rect uv) {
    const auto [a, b, c, d] = corners;
    float u2 = uv.x + uv.w, v2 = uv.y + uv.h;
    const Vertex va{a.x,a.y,1,uv.x,uv.y}, vb{b.x,b.y,1,u2,uv.y}, vc{c.x,c.y,1,u2,v2}, vd{d.x,d.y,1,uv.x,v2};
    const std::array vertices{va,vb,vc,va,vc,vd};
    const auto texture = impl_->texture(path);
    impl_->draw(texture.id, vertices, mul, add, blend, texture.premultiplied);
}

void Render::projected(std::string_view path, const std::array<Projected, 4>& corners, Color mul,
                       Color add, Blend blend, std::span<const std::vector<Vec2>> masks, Rect uv, bool nearest,
                       std::array<float, 3> hsl) {
    projected(path, corners, mul, add, blend, masks, uv, nearest, hsl, nullptr);
}

void Render::projected(std::string_view path, const std::array<Projected, 4>& corners, Color mul,
                       Color add, Blend blend, std::span<const std::vector<Vec2>> masks, Rect uv, bool nearest,
                       std::array<float, 3> hsl, const ColorMatrix* filter) {
    auto& p = *impl_;
    const auto image = path.empty() ? Impl::Texture{p.white} : p.texture(path);
    auto texture = image.id;
    if (!texture) return;
    const auto [a, b, c, d] = corners;
    const float u2 = uv.x + uv.w, v2 = uv.y + uv.h;
    const Vertex va{a.x,a.y,a.w,uv.x,uv.y}, vb{b.x,b.y,b.w,u2,uv.y},
                 vc{c.x,c.y,c.w,u2,v2}, vd{d.x,d.y,d.w,uv.x,v2};
    p.maskWindings.clear();
    if (!masks.empty() && std::all_of(corners.begin(), corners.end(), [](const auto& corner) { return corner.w > 0; })) {
        for (const auto& mask : masks) {
            const auto winding = convexWinding(mask);
            if (!winding) break;
            p.maskWindings.push_back(winding);
        }
    }
    if (!masks.empty() && p.maskWindings.size() == masks.size()) {
        // AFP's rectangle and animated quadrilateral masks intersect. Clip the
        // same homogeneous polygon successively, preserving UVs and draw order.
        auto& polygon = p.clipVertices;
        auto& vertices = p.clipTriangles;
        polygon.assign({va, vb, vc, vd});
        vertices.clear();
        for (size_t i = 0; i < masks.size(); ++i)
            clipPolygon(polygon, p.clipScratch, masks[i], p.maskWindings[i]);
        for (size_t i = 2; i < polygon.size(); ++i) {
            vertices.push_back(polygon[0]);
            vertices.push_back(polygon[i - 1]);
            vertices.push_back(polygon[i]);
        }
        p.draw(texture, vertices, mul, add, blend, image.premultiplied, nearest, false, hsl, filter);
        return;
    }
    if (!masks.empty()) {
        GLint framebuffer = p.stencilTarget;
        if (!p.stencilActive) glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        const auto transform = p.transforms.empty() ? std::array<float, 6>{1,0,0,0,1,0} : p.transforms.back();
        const bool same = p.stencilTarget == framebuffer && p.stencilClip == p.clip() &&
            p.stencilTransform == transform && std::ranges::equal(
            masks, p.stencilMasks, [](const auto& left, const auto& right) {
                return std::ranges::equal(left, right, [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; });
            });
        if (!same) {
            p.flush();
            glEnable(GL_STENCIL_TEST);
            if (p.stencilIndex + int(masks.size()) >= 255) {
                glDisable(GL_SCISSOR_TEST);
                glStencilMask(0xff);
                glClear(GL_STENCIL_BUFFER_BIT);
                p.scissor();
                p.stencilIndex = 0;
            }
            glStencilMask(0xff);
            glColorMask(false, false, false, false);
            for (size_t i = 0; i < masks.size(); ++i) {
                if (i == 0) {
                    glStencilFunc(GL_ALWAYS, ++p.stencilIndex, 0xff);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                } else {
                    glStencilFunc(GL_EQUAL, p.stencilIndex++, 0xff);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
                }
                p.draw(p.white, triangles(masks[i]), White, Transparent, Blend::Alpha);
                p.flushBatch();
            }
            glColorMask(true, true, true, true);
            p.stencilTarget = framebuffer;
            p.stencilClip = p.clip();
            p.stencilTransform = transform;
            p.stencilMasks.assign(masks.begin(), masks.end());
        } else if (!p.stencilActive) {
            p.flush();
            glEnable(GL_STENCIL_TEST);
        }
        glStencilFunc(GL_EQUAL, p.stencilIndex, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        p.stencilActive = true;
    }
    const std::array vertices{va,vb,vc,va,vc,vd};
    p.draw(texture, vertices, mul, add, blend, image.premultiplied, nearest, false, hsl, filter, !masks.empty());
}

const char* Render::fontFamily(TextFont font) { return FontSpecs[static_cast<size_t>(font)].family; }

float Render::textWidth(std::string_view text, float size, TextFont font) {
    const char* pointer = text.data();
    size_t length = text.size();
    const int pixels = std::clamp(int(std::round(size)), 1, 256);
    float width = 0.f;
    while (length)
        width += impl_->glyph(static_cast<int>(font), pixels, SDL_StepUTF8(&pointer, &length)).advance;
    return width;
}

void Render::text(std::string_view text, float x, float baseline, float size, Color color, TextAlign align,
                  float maxWidth, TextFont font, int outlineWidth, Color outlineColor) {
    std::vector<Impl::Glyph> glyphs;
    std::vector<Impl::Glyph> outlines;
    const char* pointer = text.data();
    size_t length = text.size();
    float width = 0;
    const int face = static_cast<int>(font);
    const int pixels = std::clamp(int(std::round(size)), 1, 256);
    while (length) {
        uint32_t code = SDL_StepUTF8(&pointer, &length);
        const auto glyph = impl_->glyph(face, pixels, code);
        glyphs.push_back(glyph);
        if (outlineWidth > 0) outlines.push_back(impl_->glyph(face, pixels, code, outlineWidth));
        width += glyph.advance;
    }
    float scale = maxWidth > 0 && width > maxWidth ? maxWidth / width : 1;
    if (align == TextAlign::Center) x -= width * scale / 2;
    if (align == TextAlign::Right) x -= width * scale;
    float outlineX = x;
    for (size_t i = 0; i < outlines.size(); ++i) {
        const auto& glyph = outlines[i];
        if (glyph.page) impl_->texturedRect(glyph.page, {outlineX + glyph.left * scale, baseline - glyph.top,
                                                        glyph.w * scale, glyph.h}, glyph.uv, outlineColor, Blend::Alpha);
        outlineX += glyphs[i].advance * scale;
    }
    for (const auto& glyph : glyphs) {
        if (glyph.page) impl_->texturedRect(glyph.page, {x + glyph.left * scale, baseline - glyph.top,
                                                        glyph.w * scale, glyph.h}, glyph.uv, color, Blend::Alpha);
        x += glyph.advance * scale;
    }
}

void Render::pushClip(Rect bounds) {
    auto& p = *impl_;
    p.clips.push_back(intersection(bounds, p.clip()));
}

void Render::popClip() {
    impl_->clips.pop_back();
}

Rect Render::clipBounds() const { return impl_->clip(); }

void Render::projectedStrip(std::string_view path, const std::array<Projected, 4>& strip, Rect uv,
                            Color color, Blend blend, bool glow, std::span<const Vec2> mask) {
    auto& p = *impl_;
    if (glow && !p.drawingLaserGlow) {
        p.flush();
        GLint destination = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &destination);
        p.laserGlowTarget = GLuint(destination);
        GLint color = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &color);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p.laserGlow->sourceTarget);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, GLuint(color), 0);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
            p.stageLayerDepth ? p.stageLayerTargets[p.stageLayerDepth - 1].stencil : p.stencilBuffer);
        glDisable(GL_SCISSOR_TEST);
        constexpr GLfloat clear[]{0, 0, 0, 0};
        glClearBufferfv(GL_COLOR, 1, clear);
        p.scissor();
        p.drawingLaserGlow = true;
    }
    const auto [a, b, c, d] = strip;
    const float right = uv.x + uv.w, bottom = uv.y + uv.h;
    const Vertex va{a.x, a.y, a.w, uv.x, uv.y}, vb{b.x, b.y, b.w, right, uv.y},
                 vc{c.x, c.y, c.w, uv.x, bottom}, vd{d.x, d.y, d.w, right, bottom};
    const std::array vertices{va, vb, vc, vc, vb, vd};
    const auto texture = p.texture(path);
    if (mask.empty()) {
        p.draw(texture.id, vertices, color, Transparent, blend, texture.premultiplied, false, glow);
        return;
    }
    const float winding = cross(mask[0], mask[1], mask[2]) > 0 ? 1 : -1;
    for (size_t i = 0; i < vertices.size(); i += 3) {
        auto& polygon = p.clipVertices;
        polygon.assign({vertices[i], vertices[i + 1], vertices[i + 2]});
        clipPolygon(polygon, p.clipScratch, mask, winding);
        for (size_t j = 2; j < polygon.size(); ++j) {
            const std::array triangle{polygon[0], polygon[j - 1], polygon[j]};
            p.draw(texture.id, triangle, color, Transparent, blend, texture.premultiplied, false, glow);
        }
    }
}

void Render::tunnelMesh(std::string_view path, std::span<const TunnelVertex> triangles, float opacity) {
    // Express the original 10–1000 depth range in GL homogeneous clip space.
    constexpr float near = 10, far = 1000;
    std::array<Vertex, 4 * 8 * 6> vertices;
    for (size_t i = 0; i < triangles.size(); ++i) {
        const auto& [p, uv, alpha] = triangles[i];
        const float clipZ = ((far + near) * p.w - 2 * far * near) / (far - near);
        vertices[i] = {p.x, p.y, p.w, uv.x, uv.y, clipZ, alpha};
    }
    const auto texture = impl_->texture(path);
    impl_->draw(texture.id, std::span{vertices}.first(triangles.size()), {1,1,1,opacity}, Transparent,
                Blend::Alpha, texture.premultiplied);
}

void Render::prepareLaserGlow() {
    auto& p = *impl_;
    if (!p.laserGlow) p.laserGlow = std::make_unique<LaserGlow>(p.canvas, p.stencilBuffer);
}

void Render::setPlayfieldMask(std::optional<std::span<const Vec2>> mask) { impl_->playfieldMask = mask; }

void Render::finishLaserGlow() {
    auto& p = *impl_;
    if (!p.drawingLaserGlow) return;
    p.flush();
    p.drawingLaserGlow = false;
    p.laserGlow->draw(p.laserGlowTarget, p.laserGlow->source, 1, 95.f / 255, 63.f / 255);
    p.scissor();
}

bool Render::afp(std::string_view folder, std::string_view animation, std::string_view clip, double ms,
                 bool loop, Rect bounds, const Bindings& bindings) {
    return impl_->afp.draw(*this, folder, animation, clip, ms, loop, bounds, bindings);
}

bool Render::afpOnPlane(std::string_view folder, std::string_view animation, std::string_view clip, double ms,
                        Rect source, const std::array<Projected, 3>& plane, bool loop, float opacity) {
    const auto size = impl_->afp.size(folder, animation, clip);
    return impl_->afp.draw(*this, folder, animation, clip, ms, loop,
                          {-source.x / source.w, -source.y / source.h, size.x / source.w, size.y / source.h},
                          {}, &plane, opacity);
}
double Render::afpDuration(std::string_view folder, std::string_view animation, std::string_view clip) {
    return impl_->afp.duration(folder, animation, clip);
}
Vec2 Render::afpSize(std::string_view folder, std::string_view animation, std::string_view clip) {
    return impl_->afp.size(folder, animation, clip);
}
Render::AfpAnchor Render::afpAnchor(std::string_view folder, std::string_view animation, std::string_view clip,
                                    std::string_view instance) {
    return impl_->afp.anchor(folder, animation, clip, instance);
}
void Render::prefetchAfp(std::string_view folder, std::string_view animation, std::string_view clip, bool textures) {
    prefetchAfp(folder, animation, clip, textures ? AfpTextures::Timeline : AfpTextures::None);
}
void Render::prefetchAfp(std::string_view folder, std::string_view animation, std::string_view clip, AfpTextures textures) {
    impl_->afp.prefetch(*this, folder, animation, clip, textures);
}
void Render::queueAfp(std::string_view folder, std::string_view animation, std::string_view clip) {
    impl_->afp.prefetch(*this, folder, animation, clip, AfpTextures::Clip, true);
}
void Render::queueStageAfp(std::string_view folder, std::string_view animation, std::string_view clip) {
    impl_->afp.prefetch(*this, folder, animation, clip, AfpTextures::Clip, true, true);
}
bool Render::stageImagesReady() const {
    const auto& p = *impl_;
    return p.stageImages.empty() && !std::ranges::any_of(p.decodingImages, &Impl::ImageLoad::stage);
}
void Render::prepareStageImages() { impl_->prepareQueuedImage(); }
void Render::releaseStageImages() {
    auto& p = *impl_;
    p.nextTextureTrim = p.frame;
    p.stageImages.clear();
    for (auto& image : p.decodingImages) image.stage = false;
    for (auto& [_, image] : p.textures) image.stage = false;
}
void Render::releaseStageData() { impl_->afp.releaseStageData(); }
void Render::queueAfpData(std::span<const std::string_view> folders, std::string_view extra) {
    impl_->afp.queueData(folders, extra);
}
bool Render::afpDataReady() {
    return impl_->afp.dataReady();
}
Rml::Context* Render::ui() { return impl_->context; }
void Render::drawUi(bool changed) {
    auto& p = *impl_;
    if (!changed && !p.uiResized) {
        p.texturedRect(sdvxRmlOverlayTexture, {0, 0, p.canvasW, p.canvasH}, {0, 1, 1, -1},
                       White, Blend::Alpha, true);
        return;
    }
    p.uiResized = false;
    p.flush();
    p.context->Update();
    p.uiRenderer->BeginFrame();
    p.context->Render();
    p.uiRenderer->EndFrame();
    glBindFramebuffer(GL_FRAMEBUFFER, p.framebuffer);
    p.scissor();
}
}
