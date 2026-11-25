#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct SDL_Window;
namespace Rml { class Context; }

namespace sdvx {
inline constexpr float Width = 1080;
inline constexpr float Height = 1920;
inline constexpr int SampleRate = 48000;

struct Vec2 { float x, y; };
struct Rect {
    float x, y, w, h;
    bool operator==(const Rect&) const = default;
};
struct Color {
    float r, g, b, a = 1;
    bool operator==(const Color&) const = default;
};
inline constexpr Color White{1, 1, 1, 1};
inline constexpr Color Transparent{0, 0, 0, 0};
inline constexpr Color ScoreLeadingZero{.28f, .28f, .28f, 1};
enum class Blend { Alpha, Add, Multiply };
enum class AfpTextures { None, Timeline, Clip };
enum class TextAlign { Left, Center, Right };
enum class TextFont { Body, Display, MaruGothicW4, Digital, TestMode, Korean, MaruGothicW4Fixed };
struct ImageBinding {
    std::string path;
    Rect sourceRect{};
    Rect targetRect{};
    Color tint = White;
    Color add = Transparent;
    ImageBinding() = default;
    ImageBinding(std::string value, Rect region = {}, Rect target = {}) : path(std::move(value)), sourceRect(region), targetRect(target) {}
    ImageBinding(const char* value, Rect region = {}) : path(value), sourceRect(region) {}
};
using Bindings = std::unordered_map<std::string, ImageBinding>;

std::string readAsset(std::string_view path);
std::unordered_set<std::string> externalMusicFolders();
bool hasAsset(std::string_view path);
std::string readExternalAsset(std::string_view path);
bool hasExternalAsset(std::string_view path);
double applicationTime();
std::string readSave(std::string_view name);
void writeSave(std::string_view name, std::string_view contents);

class Render {
public:
    explicit Render(SDL_Window* window);
    ~Render();
    void resetGraphics();
    void begin();
    void end();
    void flush();
    void pushTransform(const std::array<float, 6>& affine);
    void popTransform();
    void setLandscape(bool landscape);
    bool displayReady(bool landscape) const;
    void captureImage(std::string_view name, int width, int height, const std::function<void()>& draw);
    void prepareStageLayer();
    void captureStageLayer(std::string_view name, const std::function<void()>& draw);
    void filterStageLayer(std::string_view source, std::string_view destination, const std::function<void()>& effect);
    void prepareCanvasCopy(std::string_view name);
    void copyCanvas(std::string_view name);
    void prepareTunnelNoise();
    void drawTunnelNoise(std::span<const std::byte> rows, int seed, const std::function<void()>& draw);
    void stagePostEffect(float chromatic, float radial, float distortion, bool crt);
    void stagePostEffect(float chromatic, float radial, float distortion, bool crt,
                         Vec2 center, float seconds, float scanline, float darkEdge);
    void stagePostEffect(float chromatic, float radial, float distortion, bool crt,
                         Vec2 center, float seconds, float scanline, float darkEdge, Vec2 offset);
    void stageColorConversion(int mode, float strength);
    void prepareStageColorConversion();
    void prepareStagePostEffect();
    void convertStageColor(std::string_view source, std::string_view destination, int mode, float strength);
    void stageNoise(std::span<const std::byte> rows, int seed);
    void stageLuminous(float strength, float renderMax, float renderAdd);
    Vec2 logical(float x, float y) const;
    void rect(Rect rect, Color color);
    void polygon(std::span<const Vec2> vertices, Color color, Blend blend = Blend::Alpha);
    void line(Vec2 a, Vec2 b, float width, Color color);
    Vec2 imageSize(std::string_view path);
    void queueImage(std::string_view path, bool stage = false);
    void image(std::string_view path, Rect bounds, Color color = White, Blend blend = Blend::Alpha);
    void quad(std::string_view path, const std::array<Vec2, 4>& corners, Color multiply = White,
              Color add = Transparent, Blend blend = Blend::Alpha, Rect uv = {0, 0, 1, 1});
    struct Projected { float x, y, w; };
    using ColorMatrix = std::array<float, 20>;
    struct TunnelVertex { Projected position; Vec2 uv; float alpha; };
    void tunnelMesh(std::string_view path, std::span<const TunnelVertex> triangles, float opacity);
    void projected(std::string_view path, const std::array<Projected, 4>& corners, Color multiply = White,
                   Color add = Transparent, Blend blend = Blend::Alpha,
                   std::span<const std::vector<Vec2>> masks = {}, Rect uv = {0, 0, 1, 1}, bool nearest = false,
                   std::array<float, 3> hsl = {});
    void projected(std::string_view path, const std::array<Projected, 4>& corners, Color multiply,
                   Color add, Blend blend, std::span<const std::vector<Vec2>> masks, Rect uv, bool nearest,
                   std::array<float, 3> hsl, const ColorMatrix* filter);
    void projectedStrip(std::string_view path, const std::array<Projected, 4>& strip, Rect uv,
                        Color color, Blend blend, bool glow = false, std::span<const Vec2> mask = {});
    void setPlayfieldMask(std::optional<std::span<const Vec2>> mask);
    void prepareLaserGlow();
    void finishLaserGlow();
    static const char* fontFamily(TextFont font);
    float textWidth(std::string_view value, float size, TextFont font);
    void text(std::string_view value, float x, float baseline, float size, Color color = White,
              TextAlign align = TextAlign::Left, float maxWidth = 0, TextFont font = TextFont::Body,
              int outlineWidth = 0, Color outlineColor = Transparent);
    void pushClip(Rect bounds);
    void popClip();
    Rect clipBounds() const;
    bool afp(std::string_view folder, std::string_view animation, std::string_view clip,
             double ms, bool loop = false, Rect bounds = {0, 0, Width, Height},
             const Bindings& bindings = {});
    bool afpOnPlane(std::string_view folder, std::string_view animation, std::string_view clip,
                    double ms, Rect source, const std::array<Projected, 3>& plane,
                    bool loop = false, float opacity = 1);
    double afpDuration(std::string_view folder, std::string_view animation, std::string_view clip);
    Vec2 afpSize(std::string_view folder, std::string_view animation, std::string_view clip);
    struct AfpAnchor { Vec2 position; float opacity; };
    AfpAnchor afpAnchor(std::string_view folder, std::string_view animation, std::string_view clip,
                        std::string_view instance);
    void prefetchAfp(std::string_view folder, std::string_view animation, std::string_view clip, bool textures = false);
    void prefetchAfp(std::string_view folder, std::string_view animation, std::string_view clip, AfpTextures textures);
    void queueAfp(std::string_view folder, std::string_view animation, std::string_view clip);
    void queueStageAfp(std::string_view folder, std::string_view animation, std::string_view clip);
    bool stageImagesReady() const;
    void prepareStageImages();
    void releaseStageImages();
    void releaseStageData();
    void queueAfpData(std::span<const std::string_view> folders, std::string_view extra = {});
    bool afpDataReady();
    Rml::Context* ui();
    void drawUi(bool changed);

private:
    void drawStageColorConversion(unsigned int texture, int mode, float strength);
    unsigned int stagePostSource();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Audio {
public:
    using Dsp = void (*)(void*, float*, int, int64_t);
    Audio();
    ~Audio();
    bool prepareMusic(std::string_view path, bool loop = false, float gain = 1);
    bool prepareMusicData(std::string_view path, std::shared_ptr<const std::string> bytes,
                          bool loop = false, float gain = 1);
    void startMusic();
    bool playMusic(std::string_view path, bool loop = false, float gain = 1);
    void pauseMusic();
    void stopMusic();
    double positionMs() const;
    double durationMs() const;
    bool musicEnded() const;
    void setDsp(Dsp process = nullptr, void* context = nullptr);
    void sfx(std::string_view path, float gain = 1, int simultaneous = 8, double offsetMs = 0);
    void prepareSample(std::string_view path, int simultaneous = 8);
    void startEffectLoop(std::string_view path, float gain, uint64_t loopStartBytes = 0);
    void stopEffectLoop();
    void setGain(float gain);
    void setNarrationEnabled(bool enabled);
    void announce(std::string_view path, bool interrupt = true);
    void announce(std::string_view path, bool interrupt, float gain);
    void stopNarration();
    bool narrating() const;
    bool previewReady(std::string_view path, bool external);
    bool preview(std::string_view path, float gain = 1, bool loop = false, bool external = false);
    double previewPositionMs() const;
    void stopPreview(bool restoreMusic = false);
    void updatePreview();
    bool suspend(bool paused);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum Button { BTA, BTB, BTC, BTD, FXL, FXR, Start, Back, Subscreen, ButtonCount };
enum class ControllerMode { None, KeyboardMouse, Joystick };
inline constexpr double PremiumRetryHoldMs = 1000;
struct GameInput {
    enum class Type { KeyDown, KeyUp, Rotary, TouchDown, TouchMove, TouchUp, TouchCancel, Scroll, Back, FocusLost, Text, Composition };
    Type type;
    double time = 0;
    int key = 0;
    int button = -1;
    float x = 0, y = 0;
    float left = 0, right = 0;
    bool repeat = false;
    int64_t finger = 0;
    std::string text{};
    int controllerKey = 0;
};

struct Selection {
    std::string folder, chart, jacket, title, artist, difficulty, label, charter, illustrator;
    int id = 0, level = 0, bpmMin = 0, bpmMax = 0, background = 0;
    std::string music = "music.wav";
    bool externalChart = false, externalMusic = false, externalPreview = false;
    std::string preview = "preview.wav";
    int maxExscore = 0;
    std::string chartKey; // Stable difficulty key for scores and authored stage routes.
    std::string asset(std::string_view file) const { return "music/" + folder + "/" + std::string(file); }
    std::string presentation(std::string_view file) const {
        return "songs/" + std::to_string(id == 1580 ? 666 : id) + "/" + std::string(file);
    }
};

void drawLevelBadge(Render&, std::string_view difficulty, int tenths, Rect bounds);
void drawHorizontalLevel(Render&, const Selection&, Rect bounds, float numberScaleY = 1);

struct PlayerCard {
    std::string name = "LOCAL PLAYER", appealImage;
    int crew = 0, style = 0, force = 0;
    int skillLevel = 0;
    int skillTitle = 0;
};
void drawProfileCard(Render&, Rect bounds, const PlayerCard&);

inline constexpr int DefaultNemsysSkin = 47, NemsysSkinCount = 60;
struct NemsysSkin { std::string image, io; };
const NemsysSkin& nemsysSkin(int id);

struct PlayOptions {
    int trackNumber = 1;
    int courseTracks = 0;
    double permissiveRate = -1;
    double speed = 7.0;
    double displayOffset = 0;
    double judgmentOffset = 0;
    std::array<bool, 2> fineTiming{};
    double practiceUntil = 0;
    bool mirror = false, excessive = false, ultra = false, ars = false, exScore = false;
    bool autoRetry = false, sCritical = false, constantSpeed = false, autoPlay = false;
    int crew = 0;
    int cueStyle = 1;
    int brightness = 100, timingPosition = 0, leftColor = 0, rightColor = 1;
    bool timingColor = true;
    int timingMsStyle = 1, randomMode = 0, challenge = 0, target = 0, bestScore = 1;
    uint32_t randomSeed = 1;
    bool systemVoice = true, valkyrieEffect = true, showRank = true, average = false;
    int nemsys = DefaultNemsysSkin;

    double timingStep(int index) const { return fineTiming[index] ? 1. : 4.; }
    int timingLimit(int index) const { return fineTiming[index] ? 64 : 16; }
    int timingValue(int index) const {
        return static_cast<int>(std::lround((index == 0 ? judgmentOffset : displayOffset) / timingStep(index)));
    }
    void setTimingValue(int index, int value) {
        (index == 0 ? judgmentOffset : displayOffset) = std::clamp(value, -timingLimit(index), timingLimit(index)) * timingStep(index);
    }
    void toggleTimingPrecision(int index) {
        const double offset = index == 0 ? judgmentOffset : displayOffset;
        fineTiming[index] = !fineTiming[index];
        setTimingValue(index, fineTiming[index] ? static_cast<int>(std::lround(offset))
                                             : static_cast<int>(offset / timingStep(index)));
    }
};

struct Result {
    bool excessive = false, ultra = false;
    std::string outcome, grade;
    int score = 0, exScore = 0, maxExScore = 0;
    int critical = 0, sCritical = 0, near = 0, error = 0, maxChain = 0, judgments = 0;
    float gauge = 0;
    std::array<float, 100> gaugeHistory{};
    std::array<std::array<int, 3>, 3> objectCounts{};
    std::array<int, 5> timingCounts{};
    bool retry = false, cancelled = false, autoPlay = false;
};
}
