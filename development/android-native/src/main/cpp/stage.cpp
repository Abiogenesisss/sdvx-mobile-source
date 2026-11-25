#include "stage.h"
#include "stage_automation.h"
#include "s3v_audio.h"
#include "stage_scene.h"
#include "cabinet_crew_catalog.h"
#include "stage_chart.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <nlohmann/json.hpp>
#include <numbers>
#include <random>
#include <stdexcept>
#include <utility>

namespace sdvx {
namespace {

constexpr float judgeY = 1350, consoleY = 1082, laneWidth = 770, laneHeight = 1502;
constexpr float touchKeysY = judgeY + 18, touchFxY = touchKeysY + 264;
constexpr std::array<Rect, 6> touchKeys{{
    {12, touchKeysY, 264, 264}, {276, touchKeysY, 264, 264},
    {540, touchKeysY, 264, 264}, {804, touchKeysY, 264, 264},
    {12, touchFxY, 528, 184}, {540, touchFxY, 528, 184}
}};
constexpr auto touchNoteFace = "stage/touch/note.png";
constexpr std::array<std::array<std::string_view, 3>, 10> commonStageMovies{{
    {"stage_ready", "ready", "matching_out"}, {"hud", "hud_main", "hud_loop"},
    {"hud_chain", "chain", "0"}, {"button_hit", "ef_btn_all", "btn_near"},
    {"beam", "beam_01", "beam_critical"}, {"beam", "beam_02", "beam_critical"},
    {"beam", "beam_03", "beam_critical"}, {"beam", "beam_04", "beam_critical"},
    {"game_nemsys", "io", "io_in"}, {"game_nemsys", "hantei_line", "0"},
}};
constexpr std::array<std::string_view, 9> commonStageImages{
    "stage/notes_BT_chip.png", "stage/notes_FX_chip.png", "stage/notes_BT_long_lp.png",
    "stage/notes_BT_long_onlp.png", "stage/notes_FX_long_lp.png", "stage/notes_FX_long_onlp.png",
    "stage/notes_FX_long_offlp.png", "stage/notes_BT_long_offlp.png", "stage/line_01_shou.png"
};
constexpr std::array<std::string_view, 3> stageDigits{"hud/num_score_", "hud/num_chain_a_", "hud/num_chain_b_"};
constexpr std::array<std::array<std::string_view, 3>, 3> commonStageEffects{{
    {"hud_chain", "eff_chain", "eff_combo_01"},
    {"button_hit", "ef_BT_long", "FX_in"}, {"button_hit", "ef_FX_long", "FX_in"},
}};
constexpr auto touchFxFace = "stage/touch/fx.png";
constexpr float laneBackDepth = .1f;
constexpr float laneBottom = laneHeight * (1 + laneBackDepth);
constexpr float playfieldMaskBackDepth = .05f;
constexpr float playfieldMaskBottom = laneHeight * (1 + playfieldMaskBackDepth);
constexpr float rotationY = 660;
constexpr float defaultPitch = .720471919f, defaultRadius = 60.12f;
constexpr double previewBeatMs = 60000. / 130, previewLoopMs = 16 * previewBeatMs;
constexpr std::array<float, 6> buttonX{183, 318, 452, 588, 251, 520};
// GameJudgeBombView uses its own six note-space anchors at 1808d4440.
constexpr std::array<float, 6> hitAnchors{-.267f,-.089f,.089f,.267f,-.176f,.176f};
constexpr std::array<std::string_view, 6> lights{
    "io_button1", "io_button2", "io_button3", "io_button4", "io_long1", "io_long2"
};
constexpr std::array<std::string_view, 2> knobLights{"io_vol_left", "io_vol_right"};
constexpr int firstFxSample = 2, lastFxSample = 14;
// ver5/virtical_shot.s3p, slot 0: the WAV contains the unscaled PCM.
constexpr float slamSampleGain = .55132318f; // 10^((-1324 / 256 dB) / 20)
constexpr std::array<std::string_view, 5> outcomes{"comp", "crash", "uc", "puc", "spuc"};
constexpr std::array<std::string_view, 5> endingAnimations{"complete", "crash", "ultimate", "perfect", "perfect_s"};
constexpr std::array<std::string_view, 5> endingSounds{
    "005_comp", "004_crash", "003_uc", "002_puc", "001_spuc"
};
constexpr std::array<double, 5> endSoundMs{4000, 1994, 4000, 4000, 4000};
constexpr std::array<std::string_view, 6> beamAnimations{
    "beam_01", "beam_02", "beam_03", "beam_04", "beam_02", "beam_03"
};
constexpr std::array<float, 6> beamAnchors{-.265f, -.09f, .09f, .265f, -.175f, .18f};
// TumamiRenderer (2026101300, 1804533b0): centers span 0.815 of
// the lane and the textured ribbon extends 0.0925 on either side.
// These are lane-space dimensions, before perspective projection.
constexpr float laserRadius = laneWidth * .0925f;
constexpr float laserX(float position) { return laneWidth * (.0925f + .815f * position); }
struct LaserArt {
    std::array<std::string, 3> ribbon;
    std::string head;
    std::string_view holdEffect;
    std::array<std::string_view, 3> slamEffects;
};
struct LaserFx {
    double heldAt = -1, turnedAt = -1;
    const StageLaser* turn = nullptr;
};
constexpr std::array<std::string_view, 4> laserHoldEffects{
    "ef_vol_l", "ef_vol_r", "ef_cursor_green", "ef_cursor_yellow"
};
constexpr std::array<std::string_view, 2> laserReleaseEffects{"ef_vol_out_l", "ef_vol_out_r"};
constexpr std::array<std::string_view, 2> laserCriticalEffects{"ef_vol_cri_l", "ef_vol_cri_r"};
constexpr std::array<std::array<std::string_view, 3>, 2> laserCornerEffects{{
    {"ef_vo_corner_l_l", "ef_vo_corner_l_c", "ef_vo_corner_l_r"},
    {"ef_vo_corner_r_l", "ef_vo_corner_r_c", "ef_vo_corner_r_r"}
}};
constexpr std::array<std::array<std::string_view, 3>, 4> laserSlamEffects{{
    {"efhg_curve_blue", "ef_line_hg_blue", "efhg_dust_blue"},
    {"efhg_curve_red", "ef_line_hg_red", "efhg_dust_red"},
    {"efhg_curve_green", "ef_line_hg_green", "efhg_dust_green"},
    {"efhg_curve_yell", "ef_line_hg_yell", "efhg_dust_yell"}
}};
constexpr std::array<std::string_view, 2> cursorTexture{"stage/vol_cursor_left.png", "stage/vol_cursor_right.png"};
constexpr std::array<std::string_view, 2> notices{"vol_left_notice", "vol_right_notice"};
constexpr std::array<Color, 4> laserColors{{{.29f, .90f, 1}, {1, .35f, .83f}, {.48f, 1, .52f}, {1, .87f, .40f}}};
constexpr std::array<std::string_view, 5> chainMilestoneClips{
    "eff_combo_01", "eff_combo_02", "eff_combo_03", "eff_combo_04", "eff_combo_05"
};
std::string songFile(const Selection& selection, std::string_view file) {
    return file.starts_with("songs/") ? std::string(file) : selection.asset(file);
}

uint32_t littleEndian(const char* bytes, int count) {
    uint32_t number = 0;
    for (int i = 0; i < count; ++i) number |= uint32_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return number;
}

std::vector<int16_t> decodePcm(std::string_view data) {
    if (data.size() < 12 || data.compare(0, 4, "RIFF") != 0 || data.compare(8, 4, "WAVE") != 0)
        throw std::runtime_error("Invalid stage WAV");
    bool matchingFormat = false;
    for (size_t pos = 12; pos + 8 <= data.size();) {
        const auto length = littleEndian(data.data() + pos + 4, 4);
        const auto payload = pos + 8;
        if (length > data.size() - payload) break;
        const auto* bytes = data.data() + payload;
        if (data.compare(pos, 4, "fmt ") == 0 && length >= 16) {
            matchingFormat = littleEndian(bytes, 2) == 1 && littleEndian(bytes + 2, 2) == 2 &&
                littleEndian(bytes + 4, 4) == stageSampleRate && littleEndian(bytes + 14, 2) == 16;
        } else if (data.compare(pos, 4, "data") == 0 && matchingFormat && length % 4 == 0) {
            std::vector<int16_t> samples(length / 2);
            std::memcpy(samples.data(), bytes, length);
            return samples;
        }
        pos = payload + length + (length & 1);
    }
    throw std::runtime_error("Stage WAV requires 48 kHz stereo PCM");
}

struct Curve {
    double start = 0, end = 0;
    float from = 0, to = 0;
    void apply(const StageCue& cue, const std::array<float,3>* realize = nullptr) {
        const float previous = at(cue.time);
        start = cue.time;
        end = cue.end;
        const auto parameter = [&](size_t i) {
            const auto& value = cue.parameters.at(i);
            if (value.front() == 'd') return std::stof(value.substr(1)) * std::numbers::pi_v<float> / 180;
            if (value.front() == 'f') return std::stof(value.substr(1));
            const float v = std::stof(value);
            if (realize && (cue.mode & 15) >= 1 && (cue.mode & 15) <= 3)
                return (*realize)[0]*v*v + (*realize)[1]*v + (*realize)[2];
            return v;
        };
        // CameraUpdater/GeometryUpdater: mode 2 provides an explicit start;
        // the other mode continues from the value at the event time.
        from = cue.mode == 2 ? parameter(0) : previous;
        to = parameter(1);
    }
    float at(double time) const {
        if (time >= end) return to;
        if (time <= start) return from;
        return std::lerp(from, to, static_cast<float>((time - start) / (end - start)));
    }
};

struct StagePostEffects {
    Curve chromatic, radialBlur, crtDistortion, crtActive;
    Curve radialX{0, 0, .5f, .5f}, radialY{0, 0, .5f, .5f};
    Curve crtSpeed{0, 0, 1, 1}, crtScanline{0, 0, .2f, .2f}, crtDarkEdge{0, 0, 1, 1};
    Curve noiseStrength, noiseShift, noiseRandom, noiseStart, noiseEnd;
    Curve luminousStrength, luminousMax, luminousAdd;
    Curve shake;
    struct Conversion { Curve mode, strength; };
    std::array<Conversion, 2> conversion;

    bool apply(const StageCue& cue, size_t index) {
        using Member = Curve StagePostEffects::*;
        using Entry = std::pair<std::string_view, Member>;
        static constexpr Entry properties[]{
            {"ChromaticAbberationStrength", &StagePostEffects::chromatic},
            {"ChromaticAberrationStrength", &StagePostEffects::chromatic},
            {"RadicalBlurStrength", &StagePostEffects::radialBlur},
            {"RadicalBlurCenterPositionX", &StagePostEffects::radialX},
            {"RadicalBlurCenterPositionY", &StagePostEffects::radialY},
            {"CrtMonitorEffectDistortion", &StagePostEffects::crtDistortion},
            {"CrtMonitorEffectActive", &StagePostEffects::crtActive},
            {"CrtMonitorEffectSpeed", &StagePostEffects::crtSpeed},
            {"CrtMonitorEffectScanLineStrength", &StagePostEffects::crtScanline},
            {"CrtMonitorEffectDarkEdge", &StagePostEffects::crtDarkEdge},
            {"SimpleNoiseWholeNoiseStrength", &StagePostEffects::noiseStrength},
            {"SimpleNoiseWholeNoiseShift", &StagePostEffects::noiseShift},
            {"SimpleNoiseRandomNoisePercent", &StagePostEffects::noiseRandom},
            {"SimpleNoiseStartMaskRegion", &StagePostEffects::noiseStart},
            {"SimpleNoiseEndMaskRegion", &StagePostEffects::noiseEnd},
            {"LuminousEffectStrength", &StagePostEffects::luminousStrength},
            {"LuminousEffectRenderStrengthMax", &StagePostEffects::luminousMax},
            {"LuminousEffectRenderStrengthAdd", &StagePostEffects::luminousAdd},
        };
        if (cue.name == "ColorConversionMode") conversion[index].mode.apply(cue);
        else if (cue.name == "ColorConversionStrength") conversion[index].strength.apply(cue);
        else if (cue.name == "RandomShakenone")
            shake = {cue.time, cue.end, std::stof(cue.parameters[0]), std::stof(cue.parameters[1])};
        else if (const auto i = std::ranges::find(properties, cue.name, &Entry::first); i != std::end(properties))
            (this->*(i->second)).apply(cue);
        else return false;
        return true;
    }

    void drawChromatic(Render& render, double time, bool includeRadial = true) const {
        const auto chromaticStrength = chromatic.at(time);
        const auto radialStrength = includeRadial ? radialBlur.at(time) : 0;
        const auto amplitude = shake.at(time);
        if (chromaticStrength != 0 || radialStrength != 0 || amplitude != 0) {
            Vec2 offset{};
            if (amplitude != 0) {
                // 18036cfc0 independently samples a signed factor for each axis.
                std::minstd_rand random(static_cast<unsigned>(std::floor(time * .12)));
                const auto sample = [&] { return std::uniform_real_distribution<float>(-1, 1)(random) * amplitude; };
                offset = {sample() / Width, -sample() / Height};
            }
            render.stagePostEffect(chromaticStrength, radialStrength, 0, false,
                                   {radialX.at(time), 1 - radialY.at(time)}, 0, 0, 0, offset);
        }
    }

    void drawNoise(Render& render, double time) const {
        const auto wholeNoise = static_cast<uint16_t>(static_cast<int>(noiseStrength.at(time)));
        const auto wholeShift = static_cast<uint8_t>(static_cast<int>(noiseShift.at(time)));
        const auto chance = noiseRandom.at(time);
        if (wholeNoise || wholeShift || chance > 0) {
            // 180394640 adds and moves horizontal bands. 180394890 packs
            // shift, high strength and low strength in one row texel.
            using Row = std::array<uint8_t, 4>;
            std::array<Row, static_cast<size_t>(Height)> rows;
            rows.fill({wholeShift, uint8_t(wholeNoise >> 8), uint8_t(wholeNoise), 255});
            const int tick = int(std::floor(time * .12));
            for (int born = std::max(0, tick - 96); born <= tick; ++born) {
                std::minstd_rand random(1 + static_cast<unsigned>(born) * 4099);
                const auto roll = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
                if (roll(0, 100) >= noiseRandom.at(born / .12)) continue;
                const int y = roll(1, rows.size()), height = roll(1, rows.size() / 8);
                const int lifetime = roll(100, 800);
                const auto age = time - born / .12;
                if (age > lifetime) continue;
                const int top = y + int(roll(-192, 192) * age / lifetime);
                const int strength = roll(128, 9182);
                std::fill(rows.begin() + std::clamp(top, 0, int(rows.size())),
                          rows.begin() + std::clamp(top + height, 0, int(rows.size())),
                          Row{uint8_t(roll(0, 255)), uint8_t(strength >> 8), uint8_t(strength), 255});
            }
            const auto start = size_t(std::clamp(noiseStart.at(time), 0.f, 1.f) * rows.size());
            const auto end = size_t(std::clamp(noiseEnd.at(time), 0.f, 1.f) * rows.size());
            if (end > start) std::fill(rows.begin() + start, rows.begin() + end, Row{0, 0, 0, 255});
            render.stageNoise(std::as_bytes(std::span{rows}), tick % 4096);
        }
    }

    void drawCrt(Render& render, double time) const {
        if (crtActive.at(time) > 0)
            render.stagePostEffect(0, 0, crtDistortion.at(time), true, {.5f, .5f},
                                   float(time / 1000) * crtSpeed.at(time), crtScanline.at(time), crtDarkEdge.at(time));
    }

    bool active(int effect, size_t index, double time) const {
        switch (effect) {
        case 0: return chromatic.at(time) != 0 || shake.at(time) != 0;
        case 1: return conversion[index].strength.at(time) != 0;
        case 2: return radialBlur.at(time) != 0;
        case 3: return noiseStrength.at(time) != 0 || noiseShift.at(time) != 0 || noiseRandom.at(time) > 0;
        case 4: return crtActive.at(time) > 0;
        case 5: return luminousMax.at(time) > 0 || luminousAdd.at(time) > 0;
        }
        return false;
    }

    void draw(Render& render, int effect, size_t index, double time) const {
        switch (effect) {
        case 0: drawChromatic(render, time, false); break;
        case 1: render.stageColorConversion(int(conversion[index].mode.at(time)), conversion[index].strength.at(time)); break;
        case 2: render.stagePostEffect(0, radialBlur.at(time), 0, false,
                                       {radialX.at(time), 1 - radialY.at(time)}, 0, 0, 0); break;
        case 3: drawNoise(render, time); break;
        case 4: drawCrt(render, time); break;
        case 5: render.stageLuminous(luminousStrength.at(time), luminousMax.at(time), luminousAdd.at(time)); break;
        }
    }


};

struct Projection {
    float pitch, radius, shift, cosine, sine;
    float pitchCosine = std::cos(pitch), pitchSine = std::sin(pitch);
    static constexpr float focal = 1707.207184f;
    static constexpr float depthScale = 2.000400066f;

    Render::Projected project(float x, float y, int half, float height = 0) const {
        // 1804dcef0 note-space scale (61,250,755); 1804d5550 supplies
        // the transposed view/projection. The original vertex shader uses
        // its first THREE rows (v0.xyz), so w = 2.000400066 * world.z.
        // 180480029 adds (0,390); 180480097 sets the rotation pivot to 660.
        const float z = (1-y/laneHeight)*755;
        const float worldY = height*250*pitchCosine+z*pitchSine;
        const float worldZ = radius-height*250*pitchSine+z*pitchCosine;
        const float w = depthScale*worldZ;
        const float dx = focal*((x/laneWidth-.5f)*61+half*shift);
        const float dy = (judgeY-rotationY)*w-focal*worldY;
        return {Width*.5f*w+dx*cosine-dy*sine, rotationY*w+dx*sine+dy*cosine, w};
    }
    Vec2 point(float x, float y, int half, float height = 0) const {
        const auto p = project(x, y, half, height);
        return {p.x / p.w, p.y / p.w};
    }
    std::optional<Vec2> tunnelCenter() const {
        // 1804bd989 projects the anchor (0, .15, 1) through the note space.
        // 1804d5710 divides by the full projection's w, including the camera offset.
        const float y = 37.5f * pitchCosine + 755 * pitchSine;
        const float z = radius - 37.5f * pitchSine + 755 * pitchCosine;
        const float w = depthScale * z + 3;
        if (w <= 0) return std::nullopt;
        const float dy = judgeY - rotationY - focal * y / w;
        return Vec2{Width * .5f - dy * sine, rotationY + dy * cosine};
    }
    std::array<Render::Projected, 4> quad(float x1, float x2, float y1, float y2, int half, float height = 0) const {
        return {project(x1, y1, half,height), project(x2, y1, half,height), project(x2, y2, half,height), project(x1, y2, half,height)};
    }
};

struct PlayfieldMask {
    std::array<Vec2, 20> points{};
    size_t count = 0;

    static PlayfieldMask project(const Projection& camera) {
        // The six faces at 18045120a and 1804c7e96 share these eight vertices.
        // Clip edges at the camera plane before taking the silhouette.
        std::array<Render::Projected, 8> vertices;
        for (size_t index = 0; index < vertices.size(); ++index) {
            const float x = (index & 1 ? 3.5f : -2.5f) * laneWidth;
            const float y = index & 2 ? playfieldMaskBottom : 0.f;
            const float height = index & 4 ? .01f : -.01f;
            vertices[index] = camera.project(x, y, 0, height);
        }
        std::array<Vec2, 20> projected;
        size_t projectedCount = 0;
        const auto append = [&](Render::Projected point) {
            projected[projectedCount++] = {point.x / point.w, point.y / point.w};
        };
        constexpr float nearPlane = 1.f;
        for (size_t index = 0; index < vertices.size(); ++index) {
            const auto from = vertices[index];
            if (from.w >= nearPlane) append(from);
            for (const size_t bit : {1u, 2u, 4u}) {
                if (index & bit) continue;
                const auto to = vertices[index | bit];
                if ((from.w < nearPlane) == (to.w < nearPlane)) continue;
                const float t = (nearPlane - from.w) / (to.w - from.w);
                append({std::lerp(from.x, to.x, t), std::lerp(from.y, to.y, t), nearPlane});
            }
        }
        auto input = std::span{projected}.first(projectedCount);
        std::ranges::sort(input, [](Vec2 a, Vec2 b) { return a.x == b.x ? a.y < b.y : a.x < b.x; });
        const auto unique = std::ranges::unique(input, [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; });
        input = input.first(unique.begin() - input.begin());
        if (input.size() < 3) return {};
        const auto turn = [](Vec2 a, Vec2 b, Vec2 c) {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        };
        std::array<Vec2, 40> hull;
        size_t count = 0;
        for (const auto point : input) {
            while (count > 1 && turn(hull[count - 2], hull[count - 1], point) <= 0) --count;
            hull[count++] = point;
        }
        const size_t upper = count;
        for (size_t index = input.size() - 1; index-- > 0;) {
            const auto point = input[index];
            while (count > upper && turn(hull[count - 2], hull[count - 1], point) <= 0) --count;
            hull[count++] = point;
        }
        PlayfieldMask mask;
        mask.count = count - 1;
        std::copy_n(hull.begin(), mask.count, mask.points.begin());
        return mask;
    }

    std::span<const Vec2> polygon() const { return std::span{points}.first(count); }
};

void drawLaneSurface(Render& render, const Projection& projection, Color tint, Color background, float offset = 0) {
    // The separate LANECLEARCOL quad uses the stationary geometry at 1804ca050.
    const float first = std::max(0.f, offset), last = std::min(laneBottom, laneBottom + offset);
    const float band = std::clamp(laneHeight + offset, first, std::max(first, last));
    const Color rearTint{tint.r * .9f, tint.g * .9f, tint.b * .9f, tint.a};
    for (int half : {-1, 1}) {
        const float left = half < 0 ? 0.f : laneWidth / 2;
        const float right = left + laneWidth / 2;
        const float u = half < 0 ? 0.f : .5f;
        if (background.a > 0) {
            // During camera cues the rear edge can cross the camera plane.
            // Keep homogeneous coordinates so it clips with the lane texture.
            render.projected({}, projection.quad(left, right, 0, laneBottom, half), background);
        }
        if (first < band)
            render.projected("stage/lane.png", projection.quad(left, right, first, band, half),
                tint, Transparent, Blend::Alpha, {}, {u, (first - offset) / laneBottom, .5f, (band - first) / laneBottom});
        if (band < last)
            render.projected("stage/lane.png", projection.quad(left, right, band, last, half),
                rearTint, Transparent, Blend::Alpha, {}, {u, (band - offset) / laneBottom, .5f, (last - band) / laneBottom});
    }
}

void drawLaneNote(Render& render, const Projection& projection, int button, std::string_view texture,
                  float top, float bottom) {
    const float first = std::max(0.f, top), last = std::min(laneHeight, bottom);
    if (last <= first) return;
    const float half = button >= 4 ? 130 : 65, x = buttonX[button];
    const int side = button < 2 || button == 4 ? -1 : 1;
    render.projected(texture, projection.quad(x - half, x + half, first, last, side),
        White, Transparent,
        Blend::Alpha, {}, {0, (first - top) / (bottom - top), 1, (last - first) / (bottom - top)});
}

void drawLaneMeasure(Render& render, const Projection& projection, float y, double speed) {
    // 18044f8a0 draws a speed-dependent glow on XZ and a .008-high line in XY.
    constexpr auto texture = "stage/line_01_shou.png";
    const float glowDepth = .005f + .03f * static_cast<float>(std::max(0., (speed - 1) / 9));
    for (int half : {-1, 1}) {
        const float left = half < 0 ? 0.f : laneWidth / 2;
        const float right = left + laneWidth / 2;
        const float inset = laneWidth * .02f;
        render.projected(texture, projection.quad(left + (half < 0 ? inset : 0),
            right - (half > 0 ? inset : 0), y - glowDepth * laneHeight, y + .0004f * laneHeight, half),
            White, Transparent, Blend::Add, {}, {half < 0 ? .1f : .3f, .7f, .2f, .1f});
        render.projected(texture, {
            projection.project(left, y, half, .008f), projection.project(right, y, half, .008f),
            projection.project(right, y, half), projection.project(left, y, half)
        }, White, Transparent, Blend::Add, {}, {half < 0 ? 0.f : .5f, 0, .5f, 1});
    }
}

void number(Render& render, int value, int minimum, float right, float y, float height, std::string_view family) {
    std::string path = "hud/" + std::string(family) + "_0.png";
    const size_t digit = path.size() - 5;
    do {
        path[digit] = static_cast<char>('0' + value % 10);
        const auto size = render.imageSize(path);
        const auto width = size.y > 0 ? size.x * height / size.y : height * .7f;
        right -= width;
        render.image(path, {right, y, width, height});
        value /= 10;
    } while (--minimum > 0 || value > 0);
}

std::string_view beamLabel(int result, bool sCritical) {
    if (result < 2 || result > 8) return {};
    if (result == 2 || result == 8) return "beam_near";
    // GameLaneView 1804c9370 maps the inner windows to CRITICAL in normal mode.
    if (sCritical && (result == 4 || result == 6)) return "beam_s_critical";
    return "beam_critical";
}

std::string_view buttonHitLabel(int result, bool sCritical) {
    if (result == 2 || result == 8) return "btn_near";
    if (result == 4 || result == 6) return sCritical ? "btn_scritical" : "btn_critical";
    if (result == 3 || result == 5 || result == 7) return "btn_critical";
    return "btn_error";
}

std::string_view grade(int score) {
    constexpr std::array thresholds{9900000, 9800000, 9700000, 9500000, 9300000, 9000000, 8700000, 7500000, 6500000, 0};
    constexpr std::array<std::string_view, 10> names{"s", "aaa_plus", "aaa", "aa_plus", "aa", "a_plus", "a", "b", "c", "d"};
    for (size_t i = 0; i < thresholds.size(); ++i) if (score >= thresholds[i]) return names[i];
    return names.back();
}

} // namespace

const NemsysSkin& nemsysSkin(int id) {
    static const auto skins = [] {
        const auto catalog = nlohmann::json::parse(readAsset("game_nemsys/catalog.json"));
        std::array<NemsysSkin, NemsysSkinCount> result;
        for (const auto& row : catalog) {
            result.at(row.at("id").get<size_t>()) = {
                "game_nemsys/" + row.at("image").get<std::string>(), row.at("io").get<std::string>()};
        }
        return result;
    }();
    return skins[id];
}

struct Stage::SongData {
    nlohmann::json chart;
    std::shared_ptr<const std::string> music;
    std::vector<int16_t> dryPcm;
    std::shared_ptr<const StageScene::Source> scene;
};

std::shared_ptr<const Stage::SongData> Stage::prepareSong(const Selection& selection) {
    auto song = std::make_shared<SongData>();
    if (selection.id == AutomationParadise2019) {
        std::string wave;
        prepareAutomationParadise(selection, song->chart, wave, song->dryPcm, SDL_rand_bits());
        song->music = std::make_shared<const std::string>(std::move(wave));
        song->scene = StageScene::load(selection);
        return song;
    }
    const auto chart = songFile(selection, selection.chart);
    song->chart = nlohmann::json::parse(selection.externalChart ? readExternalAsset(chart) : readAsset(chart));
    const auto path = selection.asset(selection.music);
    auto bytes = selection.externalMusic ? readExternalAsset(path) : readAsset(path);
    if (path.ends_with(".s3v")) {
        bytes = decodeS3vWave(bytes, stageSampleRate);
        if (bytes.empty()) throw std::runtime_error("Unable to decode stage S3V: " + path);
    }
    song->music = std::make_shared<const std::string>(std::move(bytes));
    song->dryPcm = decodePcm(*song->music);
    song->scene = StageScene::load(selection);
    return song;
}

void Stage::queueGraphics(Render& render, const Selection&) {
    const std::array<std::string_view, 9> folders{"hud", "hud_chain", "button_hit", "beam",
        "stage_buttons", "game_nemsys", "laser_effect", "laser_notice", "stage_ready"};
    render.queueAfpData(folders);
    render.queueImage(touchNoteFace, true);
    render.queueImage(touchFxFace, true);
    for (const auto path : commonStageImages) render.queueImage(path, true);
    for (int digit = 0; digit < 10; ++digit) {
        const auto suffix = std::to_string(digit) + ".png";
        for (const auto stem : stageDigits) render.queueImage(std::string(stem) + suffix, true);
    }
}

void Stage::queuePreparedImages(Render& render, const SongData& song) {
    for (const auto& [folder, name, clip] : commonStageMovies) render.queueStageAfp(folder, name, clip);
    for (const auto& [folder, name, clip] : commonStageEffects) render.queueStageAfp(folder, name, clip);
    StageScene::queueSourceImages(render, *song.scene);
}

struct Stage::Impl {
    enum class Phase { Ready, Playing, Ending, Complete };
    Render& render;
    Audio& audio;
    Selection selection;
    PlayOptions options;
    StageChart chart;
    std::shared_ptr<const SongData> song;
    std::string jacket, title;
    PlayerCard player;
    int previousBest = 0;
    bool deferTextures;
    bool musicPrepared = false;
    StageScene scene;
    Color laneColor = White, laneClearColor = Transparent;
    struct NemsysView {
        Bindings art;
        std::array<std::string, 6> buttons;
        std::array<std::string, 2> knobs;
    };
    std::unordered_map<int, NemsysView> nemsysViews;
    const NemsysView* console = nullptr;
    Curve morph, pitch{0,0,defaultPitch,defaultPitch}, radius{0,0,defaultRadius,defaultRadius}, billboardRotation, laneY;
    std::array<StagePostEffects, 2> postEffects;
    std::array<std::optional<Curve>, 2> laserPitch;
    std::array<float,3> radiusRealize{46.5f,3.50000286f,60.12f},
        pitchRealize{.641408503f,.205076218f,.720471919f}, laserPitchRealize = pitchRealize;
    size_t nextCue = 0;
    std::array<bool, 6> buttons{};
    std::array<bool, 6> physicalButtons{};
    std::array<std::optional<int64_t>, 6> touchButtons{};
    struct TouchDial { int64_t finger; float x; };
    std::array<std::optional<TouchDial>, 2> touchDials{};
    bool touchControls = false;
    std::array<float, 2> speedRemainder{};
    bool startHeld = false;
    double shortcutAt = -1;
    std::array<double, 6> hitAt{}, feedbackAt{};
    struct TimingCue { double at = -1, offsetMs = 0; int result = 0; };
    std::array<TimingCue, 2> timingCues;
    struct HoldEffect { double started = -1, released = -1; };
    std::array<HoldEffect, 6> holdEffects;
    std::array<unsigned, 6> feedbackSeen{};
    std::array<float, 100> gaugeHistory{};
    size_t nextGauge = 0;
    Bindings hudBindings, gaugeBindings, chainBindings;
    int chainMilestone = 0;
    double chainMilestoneAt = -1;
    bool chainMilestoneGold = false;
    Phase phase = Phase::Ready;
    double clock = 0, phaseAt = -1, startedAt = 0, lastFrame = -1, postRollAt = -1;
    double readyDuration = 0, endDuration = 0, frozen = 0;
    float roll = 0;
    ClearType outcome = ClearType::Crash;
    bool forcedCrash = false;
    bool readySound = false;
    std::optional<Result> result;
    std::array<LaserArt, 2> laserArt;
    std::array<LaserFx, 2> laserFx;
    std::array<float, lastFxSample - firstFxSample + 1> fxSampleGains;
    struct ButtonSound { double due; int sample; };
    std::deque<ButtonSound> buttonSounds;
    struct SceneSound { double due; std::string path; float gain; };
    std::vector<SceneSound> sceneSounds;
    size_t nextSceneSound = 0;
    std::deque<double> slamSoundDue;
    // Long notes can overlap later short notes. Prefix maxima keep the
    // search ordered while retaining any hold whose tail is still visible.
    std::vector<double> noteTailMax;
    struct LaserGeometry {
        double trimStart, slamEnd, tailEnd, furthestTail;
        float afterSlam;
    };
    std::vector<LaserGeometry> laserGeometry;
    std::vector<double> laserSuffixStart;
    struct LaserStrip {
        std::array<Render::Projected, 4> points;
        std::string_view path;
        Rect uv;
        bool glow;
    };
    std::vector<LaserStrip> laserStrips;

    Impl(Render& graphics, Audio& sound, Selection track, PlayOptions settings, PlayerCard profileCard,
         int best, std::shared_ptr<const SongData> data, bool defer)
        : render(graphics), audio(sound), selection(std::move(track)), options(settings),
          chart(data->chart, options.mirror, options.excessive, options.ars, options.autoPlay, options.permissiveRate,
                options.randomMode, options.randomSeed, options.ultra),
          song(std::move(data)),
          jacket(songFile(selection, selection.jacket)),
          title(selection.title + " / " + selection.artist),
          player(std::move(profileCard)), previousBest(best),
          deferTextures(defer), scene(render, selection, *song->scene, defer) {
        for (int side = 0; side < 2; ++side) {
            const int material = std::clamp(side == 0 ? options.leftColor : options.rightColor, 0, 3);
            const std::string stem = "stage/notes_AD_0" + std::to_string(material);
            auto& art = laserArt[side];
            art.ribbon = {stem + "_h_lp.png", stem + "_h_on.png", stem + "_h_off.png"};
            art.head = stem + (side == 0 ? "_stL.png" : "_stR.png");
            art.holdEffect = laserHoldEffects[material];
            art.slamEffects = laserSlamEffects[material];
        }
        noteTailMax.reserve(chart.notes.size());
        double furthestTail = 0;
        for (const auto& note : chart.notes) {
            furthestTail = std::max(furthestTail, note.end);
            noteTailMax.push_back(furthestTail);
        }
        // Slam endpoints depend on the chart's scroll mode and neighbours, so
        // calculate them once. Keep the original cross-lane draw order.
        std::vector<size_t> nextOnSide(chart.lasers.size());
        std::array next{chart.lasers.size(), chart.lasers.size()};
        laserSuffixStart.resize(chart.lasers.size());
        double earliestStart = std::numeric_limits<double>::infinity();
        for (size_t i = chart.lasers.size(); i-- > 0;) {
            nextOnSide[i] = std::exchange(next[chart.lasers[i].side], i);
            // VOX segments are stored by end time. A long segment on the
            // other side can start earlier than the current segment.
            earliestStart = std::min(earliestStart, chart.lasers[i].start.time);
            laserSuffixStart[i] = earliestStart;
        }
        std::array<double, 2> trimmedUntil{};
        furthestTail = 0;
        laserGeometry.reserve(chart.lasers.size());
        for (size_t i = 0; i < chart.lasers.size(); ++i) {
            const auto& laser = chart.lasers[i];
            auto& trim = trimmedUntil[laser.side];
            if (laser.start.startsPhrase) trim = 0;
            LaserGeometry geometry{trim, laser.end.time, laser.end.time, 0, laser.end.position};
            if (laser.slam()) {
                const auto scroll = chart.scrollPosition(laser.start.time, options.constantSpeed);
                // TumamiNote 180475c80: max(20 scroll units, the next 50 ms).
                const auto length = std::max(20.0, chart.scrollPosition(laser.start.time + 50, options.constantSpeed) - scroll);
                geometry.slamEnd = chart.scrollTime(scroll + length, options.constantSpeed);
                const auto following = nextOnSide[i];
                const bool terminal = following == chart.lasers.size() || chart.lasers[following].start.startsPhrase;
                for (size_t j = following; j < chart.lasers.size(); j = nextOnSide[j]) {
                    const auto& segment = chart.lasers[j];
                    if (segment.start.startsPhrase || segment.start.time > geometry.slamEnd) break;
                    if (segment.slam()) { geometry.slamEnd = segment.start.time; break; }
                }
                for (size_t j = following; j < chart.lasers.size(); j = nextOnSide[j]) {
                    const auto& segment = chart.lasers[j];
                    if (segment.start.startsPhrase || segment.start.time > geometry.slamEnd) break;
                    if (!segment.slam()) geometry.afterSlam = segment.positionAt(chart.tickAt(geometry.slamEnd));
                    if (segment.end.time >= geometry.slamEnd) break;
                }
                trim = geometry.slamEnd;
                // A same-tick phrase tail extends at least 100 ms.
                geometry.tailEnd = terminal ? std::max(geometry.slamEnd, laser.start.time + 100) : geometry.slamEnd;
            }
            geometry.furthestTail = furthestTail = std::max(furthestTail, geometry.tailEnd);
            laserGeometry.push_back(geometry);
        }
        options.speed = std::clamp(options.speed, .5, 12.0);
        options.sCritical |= options.autoPlay;
        hitAt.fill(-1);
        feedbackAt.fill(-1);
        static constexpr std::string_view hidden[] = {
            "shape_hud_main_shape32, untextured", "shape_hud_main_shape108, untextured",
            "shape_hud_main_shape141, untextured", "shape_hud_main_shape159, untextured",
            "shape_hud_main_shape162, untextured", "shape_hud_main_shape165, untextured",
            "shape_hud_main_shape168, untextured", "shape_hud_main_shape171, untextured",
            "shape_hud_main_shape174, untextured", "shape_hud_main_shape63, untextured",
            "pt_option_win", "pt_result_icon_none", "pt_result_icon_puc", "blpass_base", "blpass_ef",
            "gauge_effective_stagemain"
        };
        for (auto name : hidden) hudBindings.emplace(name, "");
        const bool finalTrack = options.courseTracks ? options.trackNumber == options.courseTracks : options.trackNumber >= 3;
        const auto trackArt = options.practiceUntil > 0 ? "06" : !options.courseTracks && options.trackNumber == 4 ? "05" : finalTrack ? "04"
            : options.trackNumber == 3 ? "03" : options.trackNumber == 2 ? "02" : "01";
        hudBindings["hud_track_06"] = "hud/hud_track_" + std::string(trackArt) + ".png";
        hudBindings["jk_1580_5_ss"] = jacket;
        gaugeBindings.emplace("shape_gauge_effective_stagemain_shape9, untextured", "");
        gaugeBindings.emplace("shape_gauge_excessive_stagemain_shape9, untextured", "");
        gaugeBindings.emplace("shape_gauge_permissive_stagemain_shape9, untextured", "");
        gaugeBindings.emplace("shape_gauge_ultra_stagemain_shape9, untextured", "");
        readyDuration = render.afpDuration("stage_ready", "ready", "matching_out");
        scene.queueInitial(chart.cues);
        for (const auto& cue : chart.cues) {
            const std::string_view name = cue.name;
            if (name == "ChromaticAbberationStrength" || name == "ChromaticAberrationStrength"
                || name == "RadicalBlurStrength" || name == "CrtMonitorEffectActive" || name == "RandomShakenone")
                render.prepareStagePostEffect();
            else if (name == "ColorConversionStrength")
                render.prepareStageColorConversion();
        }
        console = &prepareNemsys(options.nemsys);
        for (auto& cue : chart.cues) {
            if (cue.name != "SpecialN" || cue.parameters.size() < 2) continue;
            const auto& target = cue.parameters[0];
            if (target == "SE" || (target == "SE2" && cue.parameters.size() >= 4
                && cue.parameters[2] == "PLAY_IIDX_AREA_BIG")) {
                auto path = "stage/sound/" + cue.parameters[1] + ".wav";
                const float gain = target == "SE" ? 1.f : std::stof(cue.parameters[3].substr(1));
                audio.prepareSample(path, 1);
                sceneSounds.push_back({cue.time, std::move(path), gain});
            }
            if (target != "NEMSYS" || cue.parameters.size() < 3) continue;
            // The second integer restores the stage's initial skin instead of using the ID.
            int id = options.nemsys;
            if (cue.parameters[2] != "i1") {
                const std::string_view value = cue.parameters[1];
                if (!value.starts_with('i')) continue;
                const auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), id);
                if (error != std::errc{} || end != value.data() + value.size() || id < 0 || id >= NemsysSkinCount) continue;
            }
            cue.nemsys = id;
            prepareNemsys(id);
        }
        if (options.valkyrieEffect && !chart.lasers.empty()) render.prepareLaserGlow();
        if (std::ranges::any_of(chart.lasers, &StageLaser::slam))
            audio.prepareSample("stage/sound/virtical_shot.wav", 1);
        static const auto sampler = nlohmann::json::parse(readAsset("stage/sound/general_sampler.json"));
        std::array<bool, lastFxSample - firstFxSample + 1> usedSamples{};
        for (const auto& note : chart.notes)
            if (note.sample >= firstFxSample && note.sample <= lastFxSample)
                usedSamples[note.sample - firstFxSample] = true;
        for (int slot = firstFxSample; slot <= lastFxSample; ++slot) {
            const float db = sampler.at(slot).at("s3v_gain_db");
            fxSampleGains[slot - firstFxSample] = std::pow(10.f, db / 20.f);
            if (usedSamples[slot - firstFxSample]) audio.prepareSample("stage/sound/" + std::to_string(slot) + ".wav", 1);
        }
        queueSceneGraphics();
    }

    const NemsysView& prepareNemsys(int id) {
        auto [entry, inserted] = nemsysViews.try_emplace(id);
        auto& view = entry->second;
        if (!inserted) return view;
        const auto& skin = nemsysSkin(id);
        view.art.emplace("io_main_i_eeb", skin.image);
        const std::string suffix = skin.io == "v" ? "" : "_" + skin.io;
        if (!suffix.empty()) {
            const std::string judge = skin.io == "i" ? "ii" : skin.io;
            view.art.emplace("io_front_v", "game_nemsys/io_front" + suffix + ".png");
            view.art.emplace("hantei_line_v", "game_nemsys/hantei_line_" + judge + ".png");
            view.art.emplace("hantei_kikai_ef_v", "game_nemsys/hantei_kikai_ef_" + judge + ".png");
        }
        for (size_t i = 0; i < lights.size(); ++i) view.buttons[i] = std::string(lights[i]) + suffix;
        for (size_t i = 0; i < knobLights.size(); ++i) view.knobs[i] = std::string(knobLights[i]) + suffix;
        return view;
    }

    double chartTime(double now) const {
        if (phase != Phase::Playing) return frozen;
        if (postRollAt >= 0) return audio.durationMs() + std::max(0.0, now - postRollAt);
        return std::max(frozen, audio.positionMs());
    }

    void queueButtonSound(int number, double due) {
        if (number < firstFxSample || number > lastFxSample) return;
        // Both FX lanes can call the same single-voice sampler slot on one chart tick.
        constexpr double sameTick = .000001;
        const auto position = std::ranges::upper_bound(buttonSounds, due, {}, &ButtonSound::due);
        for (auto it = position; it != buttonSounds.begin() && std::prev(it)->due >= due - sameTick;)
            if ((--it)->sample == number) return;
        for (auto it = position; it != buttonSounds.end() && it->due <= due + sameTick; ++it)
            if (it->sample == number) return;
        buttonSounds.insert(position, {due, number});
    }

    void hitSlam(const StageLaser& laser, double musicTime, double now) {
        // 1804728d2 gives GameAudio the same offset used by the judge clock.
        // 1804b8a86 subtracts it from the hit delta, leaving the music-clock delta.
        slamSoundDue.push_back(std::floor(now) - std::trunc(musicTime - laser.start.time));
        // Event 12 resets the HG animation when consumed (180436bb0).
        auto& fx = laserFx[laser.side];
        fx.turnedAt = now;
        fx.turn = &laser;
    }

    void stopInputs(double time) {
        if (phase == Phase::Playing && !options.autoPlay) chart.clearInputs(time);
        buttons.fill(false);
        physicalButtons.fill(false);
        touchButtons.fill(std::nullopt);
        touchDials.fill(std::nullopt);
        startHeld = false;
        speedRemainder.fill(0);
        shortcutAt = -1;
    }

    void setButton(int button, bool down, double time, double inputAt, double now) {
        if (buttons[button] == down) return;
        buttons[button] = down;
        if (phase != Phase::Playing || options.autoPlay) return;
        if (down) {
            const auto [result, sample, offset] = chart.press(button, time);
            if (offset && result != judgmentCritical) {
                const bool near = result == 2 || result == 8;
                const bool outerCritical = result == 3 || result == 7;
                const std::array styles{options.cueStyle, options.timingMsStyle};
                for (size_t i = 0; i < styles.size(); ++i) {
                    const int style = styles[i];
                    if (style == 3 || (style > 0 && near) || (style == 2 && outerCritical))
                        timingCues[i] = {inputAt, *offset, result};
                }
            }
            if (result >= 2 && result <= 8) hitAt[button] = now;
            if (sample) queueButtonSound(sample, time + *offset);
            if (result != 0) SDL_Log("SDVX Input lane=%d result=%d score=%d", button, result, chart.score());
        } else {
            chart.release(button, time);
        }
        syncJudgmentFeedback(now);
    }

    void syncJudgmentFeedback(double now) {
        for (int i = 0; i < 6; ++i) {
            const auto sequence = chart.feedbackSequence(i);
            if (feedbackSeen[i] != sequence) {
                feedbackSeen[i] = sequence;
                feedbackAt[i] = now;
                if (options.autoPlay) hitAt[i] = now;
            }
            auto& hold = holdEffects[i];
            if (chart.holding(i)) {
                if (hold.started < 0) hold = {now, -1};
            } else if (hold.started >= 0) hold = {-1, now};
        }
        // GameJudgeComboView binds the current chain to the milestone movie.
        const auto& stats = chart.stats();
        const int milestone = stats.maxChain / 100;
        if (milestone > chainMilestone) {
            chainMilestone = milestone;
            chainMilestoneAt = now;
            chainMilestoneGold = stats.errors == 0;
        }
    }

    void finish(double now, bool challengeFailed = false) {
        frozen = chartTime(now);
        audio.pauseMusic();
        phase = Phase::Ending;
        phaseAt = now;
        outcome = chart.clearType(options.sCritical, forcedCrash || challengeFailed);
        const auto index = static_cast<size_t>(outcome);
        endDuration = std::max(render.afpDuration("stage_end", endingAnimations[index], "0"), endSoundMs[index]);
        audio.sfx("stage_end/se_07_comp" + std::string(endingSounds[index]) + ".wav");
        static constexpr std::array voices{"comp01.wav", "comp07.wav", "comp02.wav", "comp03.wav", "comp04.wav"};
        if (options.systemVoice)
            if (const auto* voice = crewVoice(options.crew, voices[index])) audio.announce(voice->path, true, voice->gain);
        SDL_Log("SDVX Stage ended %s score=%d gauge=%.4f chain=%d judgments=%d", outcomes[index].data(), chart.score(),
            chart.stats().gauge, chart.stats().maxChain, chart.stats().critical + chart.stats().near + chart.stats().errors);
    }

    void makeResult(double now) {
        const auto& stats = chart.stats();
        Result report;
        report.autoPlay = options.autoPlay;
        report.outcome = outcomes[static_cast<size_t>(outcome)];
        report.score = chart.score();
        report.grade = grade(report.score);
        report.exScore = chart.exScore();
        report.maxExScore = stats.maxEx;
        report.critical = stats.critical;
        report.sCritical = chart.judgments()[0][4] + chart.judgments()[0][6];
        report.near = stats.near;
        report.error = stats.errors;
        report.maxChain = stats.maxChain;
        report.judgments = stats.total;
        report.gauge = stats.gauge;
        report.excessive = stats.excessive;
        report.ultra = stats.ultra;
        while (nextGauge < gaugeHistory.size()) gaugeHistory[nextGauge++] = stats.gauge;
        report.gaugeHistory = gaugeHistory;
        const auto& counts = chart.judgments();
        for (size_t kind = 0; kind < counts.size(); ++kind) {
            for (int value = 1; value <= 10; ++value) {
                const int count = counts[kind][value];
                const int bucket = value == 1 || value == 10 ? 2 : value == 2 || value == 8 ? 1 : 0;
                const int timing = value == 1 ? 0 : value == 2 ? 1 : value == 8 ? 3 : value == 10 ? 4 : 2;
                report.objectCounts[kind][bucket] += count;
                report.timingCounts[timing] += count;
            }
        }
        report.retry = now < options.practiceUntil && options.autoRetry && outcome == ClearType::Crash;
        result = std::move(report);
        phase = Phase::Complete;
    }

    void quickRetry() {
        result = Result{};
        result->retry = true;
        phase = Phase::Complete;
    }

    void applyCue(const StageCue& cue) {
        if (cue.name == "Realize" && (cue.mode == 3 || cue.mode == 4 || cue.mode == 6 || cue.mode == 7)) {
            const float low=std::stof(cue.parameters[0]), center=std::stof(cue.parameters[1]), high=std::stof(cue.parameters[2]);
            // 1802da5b0 and 1802da660 share one table for AIRL and AIRR.
            auto& coefficients = cue.mode == 3 ? radiusRealize : cue.mode == 4 ? pitchRealize : laserPitchRealize;
            coefficients = {low-center+(high-low)*.5f,(high-low)*.5f,center};
        } else if (cue.name == "Morphing2") morph.apply(cue);
        else if (cue.name == "CAM_RotX") pitch.apply(cue,&pitchRealize);
        else if (cue.name == "CAM_Radi") radius.apply(cue,&radiusRealize);
        else if (cue.name == "AIRL_RotX" || cue.name == "AIRR_RotX") {
            auto& curve = laserPitch[(cue.name == "AIRR_RotX") ^ options.mirror];
            if (!curve) {
                const float current = pitch.at(cue.time);
                curve = Curve{0, 0, current, current};
            }
            curve->apply(cue, &laserPitchRealize);
        }
        else if (cue.name == "BIL_RotZ") billboardRotation.apply(cue);
        else if (cue.name == "LaneY") laneY.apply(cue);
        else if (cue.post) postEffects[cue.post->layer].apply(cue, cue.post->index);
        else if (postEffects[0].apply(cue, 0)) return;
        else if (cue.name == "SpecialN" && cue.parameters.size() >= 2) {
            const auto& target = cue.parameters[0];
            const auto& event = cue.parameters[1];
            if (cue.nemsys) {
                console = &nemsysViews.at(*cue.nemsys);
            } else if (target == "LANECOL" || target == "LANECLEARCOL") {
                if (!event.starts_with('x') && !event.starts_with('i')) return;
                const auto first = event.data() + 1;
                const auto last = event.data() + event.size();
                uint32_t packed = 0;
                if (event.starts_with('x')) {
                    const auto [end, error] = std::from_chars(first, last, packed, 16);
                    if (error != std::errc{} || end != last) return;
                } else {
                    // The EXHAUST chart for Λkasha stores these same ARGB bits as signed decimals.
                    int32_t value = 0;
                    const auto [end, error] = std::from_chars(first, last, value);
                    if (error != std::errc{} || end != last) return;
                    packed = static_cast<uint32_t>(value);
                }
                auto& color = target == "LANECOL" ? laneColor : laneClearColor;
                color = {((packed >> 16) & 255) / 255.f, ((packed >> 8) & 255) / 255.f,
                         (packed & 255) / 255.f, (packed >> 24) / 255.f};
            } else {
                scene.cue(cue);
            }
        } else {
            scene.cue(cue);
        }
    }

    enum class ConsoleLayer { JudgeLine, ButtonsAndKnobs, ButtonHits, LaserHits, LaserCursor, Body };

    void drawOriginalStage(double now, double time, const Projection& projection, float autoTilt, float angle) {
        const auto center = projection.tunnelCenter();
        constexpr int playfield = 55, brightness = 53, judgeLine = 52, hitEffects = 51;
        constexpr int cursor = 49, console = 48, controls = 47, combo = 45, comboMilestone = 44;
        constexpr int hud = 41, hudTrack = 39, ending = 37, ready = 35;
        constexpr std::array gameplay{playfield, brightness, judgeLine, hitEffects, cursor,
                                       console, controls, combo, comboMilestone, hud, hudTrack, ending, ready};
        std::array<int, 80> order;
        auto end = std::ranges::copy(gameplay, order.begin()).out;
        end = std::ranges::copy(scene.drawPriorities(), end).out;
        const auto passes = scene.postPasses();
        using Output = StagePostPass::Output;
        const auto active = [&](size_t index) {
            if (passes[index].output != Output::Filtered) return scene.postMovieActive(index);
            size_t conversionIndex = 0;
            return std::ranges::any_of(passes[index].effects, [&](int effect) {
                return postEffects[index].active(effect, effect == 1 ? conversionIndex++ : 0, time);
            });
        };
        std::array<bool, 2> enabled{};
        for (size_t index = 0; index < passes.size(); ++index) {
            enabled[index] = active(index);
            if (!enabled[index] && passes[index].output != Output::Inversion) continue;
            if (enabled[index]) *end++ = passes[index].outputPriority;
            *end++ = passes[index].firstPriority;
            *end++ = passes[index].endPriority - 1;
        }
        std::sort(order.data(), end, std::greater<>());
        end = std::unique(order.data(), end);
        const auto draw = [&](auto&& self, int ceiling, int floor, unsigned capturing) -> void {
            for (auto entry = order.data(); entry != end; ++entry) {
                const int priority = *entry;
                if (priority > ceiling || priority < floor) continue;
                bool rangesCaptured = false;
                for (size_t index = 0; index < passes.size(); ++index) {
                    const auto& pass = passes[index];
                    if ((!enabled[index] && pass.output != Output::Inversion) ||
                        (capturing & (1u << index)) || pass.endPriority - 1 != priority) continue;
                    render.captureStageLayer(StageCaptureTargets[index], [&] {
                        self(self, priority, pass.firstPriority, capturing | (1u << index));
                    });
                    if (!enabled[index]) {
                        render.quad(StageCaptureTargets[index], {{{0, 0}, {Width, 0}, {Width, Height}, {0, Height}}},
                                    White, Transparent, Blend::Alpha, {0, 1, 1, -1});
                    }
                    while (entry + 1 != end && entry[1] >= pass.firstPriority) ++entry;
                    rangesCaptured = true;
                    break;
                }
                if (!rangesCaptured) {
                    scene.drawAtPriorities(time, center, projection.pitch, autoTilt, angle, priority, priority + 1);
                    switch (priority) {
                    case playfield: drawPlayfield(now, time, projection); break;
                    case brightness:
                        if (options.brightness < 100)
                            render.rect({0, 0, Width, Height}, {0, 0, 0, (100 - options.brightness) * .01f});
                        break;
                    case judgeLine: drawConsole(now, projection, time, ConsoleLayer::JudgeLine); break;
                    case hitEffects:
                        drawConsole(now, projection, time, ConsoleLayer::ButtonHits);
                        drawConsole(now, projection, time, ConsoleLayer::LaserHits);
                        break;
                    case cursor: drawConsole(now, projection, time, ConsoleLayer::LaserCursor); break;
                    case console: drawConsole(now, projection, time, ConsoleLayer::Body); break;
                    case controls: drawConsole(now, projection, time, ConsoleLayer::ButtonsAndKnobs); break;
                    case combo: drawCombo(now, false); break;
                    case comboMilestone:
                        drawCombo(now, true);
                        drawJudgments(now);
                        break;
                    case hud: drawHud(now); break;
                    case hudTrack: drawHudTrack(); break;
                    case ending:
                        if (endDuration > 0)
                            render.afp("stage_end", endingAnimations[static_cast<size_t>(outcome)], "0",
                                       std::max(0.0, now - phaseAt));
                        break;
                    case ready:
                        if (phase == Phase::Ready)
                            render.afp("stage_ready", "ready", "matching_out", phaseAt < 0 ? 0 : std::max(0.0, now - phaseAt));
                        break;
                    }
                }
                const int output = *entry;
                for (size_t index = 0; index < passes.size(); ++index) {
                    const auto& pass = passes[index];
                    if (!enabled[index] || (capturing & (1u << index)) || pass.outputPriority != output) continue;
                    const auto& effect = postEffects[index];
                    std::string_view source = StageCaptureTargets[index];
                    if (pass.output == Output::Inversion) {
                        scene.drawPostInversion(index, source);
                    } else if (pass.output == Output::Movie) {
                        scene.drawPostMovie(source, [&](size_t step, std::string_view input, std::string_view target) {
                            const int type = pass.effects[step];
                            if (!effect.active(type, 0, time)) return false;
                            render.filterStageLayer(input, target, [&] { effect.draw(render, type, 0, time); });
                            return true;
                        });
                    } else {
                        size_t conversionIndex = 0, nextTarget = 0;
                        for (const int type : pass.effects) {
                            const auto parameter = type == 1 ? conversionIndex++ : 0;
                            if (!effect.active(type, parameter, time)) continue;
                            const auto target = StageFilterTargets[nextTarget++ % StageFilterTargets.size()];
                            render.filterStageLayer(source, target, [&] { effect.draw(render, type, parameter, time); });
                            source = target;
                        }
                        render.quad(source, {{{0, 0}, {Width, 0}, {Width, Height}, {0, Height}}},
                                    White, Transparent, Blend::Alpha, {0, 1, 1, -1});
                    }
                }
            }
        };
        draw(draw, order.front(), end[-1], 0);
    }

    void queueSceneGraphics() {
        const auto image = [&](std::string_view path) {
            if (deferTextures) render.queueImage(path, true);
            else render.imageSize(path);
        };
        const auto movie = [&](std::string_view folder, std::string_view name, std::string_view clip) {
            if (deferTextures) render.queueStageAfp(folder, name, clip);
            else render.prefetchAfp(folder, name, clip, AfpTextures::Clip);
        };
        // Late encounter hits and releases use the same stage residency as the initial effects.
        scene.queueGraphics(chart.cues);
        for (const auto& [folder, name, clip] : commonStageMovies) movie(folder, name, clip);
        movie("hud", "hud_time", "music_start");
        movie("hud", "small_1_stagemain", "0");
        movie("hud", options.permissiveRate >= 0 ? "gauge_permissive_stagemain"
            : options.ultra ? "gauge_ultra_stagemain"
            : options.excessive ? "gauge_excessive_stagemain" : "gauge_effective_stagemain", "0_per");
        for (const auto& [_, binding] : hudBindings)
            if (!binding.path.empty() && !binding.path.starts_with("runtime/")) image(binding.path);
        if (options.permissiveRate >= 0) image("hud/gauge_permissiv.png");
        else if (options.ultra) image(options.ars ? "hud/gauge_ultra_ars.png" : "hud/gauge_ultra.png");
        else if (options.excessive) image("cabinet/gauge_excessive.png");
        for (const auto& [folder, name, clip] : commonStageEffects) movie(folder, name, clip);
        if (options.valkyrieEffect) movie("button_hit", "lane_effect", "0");
        for (const auto& [_, view] : nemsysViews) {
            for (const auto& entry : view.art) if (!entry.second.path.empty()) image(entry.second.path);
            for (const auto& name : view.buttons) movie("stage_buttons", name, "btn_hit");
            for (const auto& name : view.knobs) movie("stage_buttons", name, "vol_hit_in");
        }
        for (const auto path : commonStageImages) image(path);
        for (int digit = 0; digit < 10; ++digit) {
            const auto suffix = std::to_string(digit) + ".png";
            for (const auto stem : stageDigits) image(std::string(stem) + suffix);
            if (previousBest > 0) image("hud/num_score_small_" + suffix);
        }
        if (chart.lasers.empty()) return;
        const bool slams = std::ranges::any_of(chart.lasers, &StageLaser::slam);
        for (size_t side = 0; side < laserArt.size(); ++side) {
            const auto& art = laserArt[side];
            for (const auto& path : art.ribbon) image(path);
            image(art.head);
            image(cursorTexture[side]);
            movie("laser_notice", notices[side], "0");
            movie("laser_effect", laserCriticalEffects[side], "0");
            movie("laser_effect", laserReleaseEffects[side], "0");
            for (const auto name : laserCornerEffects[side]) movie("laser_effect", name, "0");
            movie("laser_effect", art.holdEffect, art.holdEffect.starts_with("ef_vol_") ? "loop" : "0");
            if (slams)
                for (const auto name : art.slamEffects) movie("laser_effect", name, "0");
        }
    }

    enum class PlayfieldLayer { LaserUnderlay, LaneEffects, FxHolds, FxHoldEffects, BtHoldEffects, Notes, Lasers };

    void drawPlayfield(double now, double time, const Projection& projection) {
        // 1804cccaf registers the output of GameSceneRenderer at priority 55.
        // Note, laser and beam priorities are local to that renderer.
        const bool ready = phase == Phase::Ready;
        // 1804c8730 passes the same -1.25 * LaneY / 1024 Z translation to
        // 1804c9c10 for the textured board and the lane_effect AFP.
        const float offset = ready ? 0.f : laneHeight * (1.25f * laneY.at(time) / 1024);
        drawLaneSurface(render, projection, laneColor, laneClearColor, offset);
        if (ready) return;
        const auto mask = PlayfieldMask::project(projection);
        render.setPlayfieldMask(mask.polygon());
        drawLane(projection, time, now, PlayfieldLayer::LaserUnderlay);
        const float far = laneHeight * (1 - .275f) + offset;
        const float near = laneHeight * (1 + .075f) + offset;
        // GameLaneView 1804c9920 applies LANECOL to the board and lane_effect.
        // Alpha-driven disappearances must fade the attached light with it.
        if (options.valkyrieEffect && laneColor.a > 0 && far < playfieldMaskBottom && near > 0) {
            for (int half : {-1, 1}) {
                const float left = half < 0 ? 0.f : laneWidth / 2;
                const float right = left + laneWidth / 2;
                render.afpOnPlane("button_hit", "lane_effect", "0", now - startedAt,
                    {half < 0 ? 0.f : 384.f, 0, 384, 1500},
                    {projection.project(left, far, half), projection.project(right, far, half),
                     projection.project(left, near, half)}, true, .9f * laneColor.a);
            }
        }
        drawLane(projection, time, now, PlayfieldLayer::LaneEffects);
        render.setPlayfieldMask(std::nullopt);
        drawLane(projection, time, now, PlayfieldLayer::FxHolds);
        drawLane(projection, time, now, PlayfieldLayer::FxHoldEffects);
        drawLane(projection, time, now, PlayfieldLayer::BtHoldEffects);
        drawLane(projection, time, now, PlayfieldLayer::Notes);
        render.setPlayfieldMask(mask.polygon());
        drawLane(projection, time, now, PlayfieldLayer::Lasers);
        render.setPlayfieldMask(std::nullopt);
    }

    void drawLane(const Projection& projection, double drawTime, double now, PlayfieldLayer layer) {
        if (layer == PlayfieldLayer::Lasers) {
            const Color tint{1, 1, 1, options.valkyrieEffect ? .75f : .95f};
            for (const auto& strip : laserStrips)
                render.projectedStrip(strip.path, strip.points, strip.uv, tint, Blend::Add, strip.glow);
            render.finishLaserGlow();
            return;
        }
        // HudSpeed (180444ff0) displays the target speed / 100. DrawRangeEx
        // (18014de40) uses 3640 - 240 scroll units in front of the clock.
        const auto speed = options.speed * 100 / chart.scrollBpm(options.constantSpeed);
        const auto origin = chart.scrollPosition(drawTime, options.constantSpeed);
        const auto span = 3400 / speed;
        const auto visibleUntil = chart.scrollTime(origin + span, options.constantSpeed);
        // 18045120a and 1804c7e96 set the local masks to z=1 through z=-.05.
        const auto laserNear = chart.scrollTime(origin - playfieldMaskBackDepth * span, options.constantSpeed);
        const auto noteY = [&](double ms) {
            return static_cast<float>((1 - (chart.scrollPosition(ms, options.constantSpeed) - origin) / span) * laneHeight);
        };
        if (layer == PlayfieldLayer::LaserUnderlay) {
            std::array<Projection, 2> laserProjections{projection, projection};
            for (int side = 0; side < 2; ++side) {
                if (laserPitch[side]) laserProjections[side] = {
                    laserPitch[side]->at(drawTime), projection.radius, projection.shift, projection.cosine, projection.sine};
            }
            const Color shadowTint{0, 0, 0, options.valkyrieEffect ? .75f : .95f};
            laserStrips.clear();
            const auto first = std::ranges::lower_bound(laserGeometry, laserNear, {}, &LaserGeometry::furthestTail);
            const auto last = std::ranges::upper_bound(laserSuffixStart, visibleUntil) - laserSuffixStart.begin();
            for (auto index = first - laserGeometry.begin(); index < last; ++index) {
                const auto& laser = chart.lasers[index];
                if (laser.start.time > visibleUntil) continue;
                const auto& [trimStart, slamEnd, tailEnd, furthestTail, afterSlam] = laserGeometry[index];
                const bool slam = laser.slam();
                const double from = std::max({laserNear, laser.start.time, trimStart});
                const double to = std::min(visibleUntil, laser.end.time);
                if (slam ? tailEnd <= laserNear : from > to) continue;
                const auto& art = laserArt[laser.side];
                const auto& laserProjection = laserProjections[laser.side];
                const auto drawMesh = [&](const std::array<Vec2, 4>& strip, std::string_view texture = {}, Rect uv = {0,.4f,1,.2f}) {
                    // TumamiNote uses the shared chart X axis through Morphing2.
                    std::array<Render::Projected, 4> points;
                    for (size_t i = 0; i < points.size(); ++i)
                        points[i] = laserProjection.project(strip[i].x, strip[i].y, 0, .01f);
                    // Group 48 draws the black _h_off underlay. Groups 32
                    // and 31 draw the laser color with the same mask.
                    const bool body = texture.empty();
                    const int state = body ? chart.laserMaterial(laser) : 0;
                    render.projectedStrip(body ? laserArt[1].ribbon[2] : texture, points, uv, shadowTint, Blend::Alpha);
                    laserStrips.push_back({points, body ? art.ribbon[state] : texture, uv,
                                           options.valkyrieEffect && body && state != 2});
                };
                if (laser.start.phraseHead && from == laser.start.time) {
                    const float position = laser.start.position;
                    const float x = laserX(position), y = noteY(from);
                    constexpr float headLength = laneHeight * .1f;
                    const float height = std::min(headLength, playfieldMaskBottom - y);
                    if (height > 0) {
                        drawMesh({{{x-laserRadius,y},{x+laserRadius,y},{x-laserRadius,y+height},{x+laserRadius,y+height}}},
                            art.head, {0, .1f, 1, .9f * height / headLength});
                    }
                }
                if (slam) {
                    const float shift = laser.direction() > 0 ? -laneWidth * .001f : 0;
                    const float a = laserX(laser.start.position) + shift;
                    const float b = laserX(laser.end.position) + shift;
                    const float c = laserX(afterSlam) + shift;
                    const float nearY = noteY(laser.start.time), farY = noteY(slamEnd);
                    const float r = laser.direction() < 0 ? laserRadius : -laserRadius;
                    // 1804533b0 emits two corner triangles and the joining strip.
                    // Rotating and stretching one rectangle loses these joins.
                    if (slamEnd > laserNear) {
                        drawMesh({{{a+r,farY},{a+r,farY},{a-r,nearY},{a+r,nearY}}});
                        drawMesh({{{c-r,farY},{c+r,farY},{b-r,nearY},{b-r,nearY}}});
                        drawMesh({{{c+r,farY},{b-r,nearY},{a+r,farY},{a-r,nearY}}});
                    }
                    const auto tailFrom = std::max(slamEnd, laserNear);
                    const auto tailTo = std::min(tailEnd, visibleUntil);
                    if (tailFrom < tailTo) {
                        const float x = laserX(laser.end.position);
                        drawMesh({{{x-laserRadius,noteY(tailTo)},{x+laserRadius,noteY(tailTo)},
                                   {x-laserRadius,noteY(tailFrom)},{x+laserRadius,noteY(tailFrom)}}});
                    }
                    continue;
                }
                const float fromY = noteY(from), toY = noteY(to);
                if (fromY - toY < .25f) continue;
                const auto near = laser.positionAt(chart.tickAt(from));
                const auto far = laser.positionAt(chart.tickAt(to));
                drawMesh({{{laserX(far)-laserRadius,toY},{laserX(far)+laserRadius,toY},
                           {laserX(near)-laserRadius,fromY},{laserX(near)+laserRadius,fromY}}});
            }
            return;
        }
        if (layer == PlayfieldLayer::LaneEffects) {
            for (size_t i = 0; i < beamAnimations.size(); ++i) {
                const double elapsed = now - feedbackAt[i];
                const auto label = beamLabel(chart.feedback(i), options.sCritical);
                const auto animation = beamAnimations[i];
                if (feedbackAt[i] < 0 || label.empty() || elapsed >= render.afpDuration("beam", animation, label)) continue;
                const bool fx = i >= 4;
                const int half = i < 2 || i == 4 ? -1 : 1;
                // GameLaneView 1804c5fb0 crops each beam in a quarter-scale AFP
                // atlas, then places its quad in XZ (1804d6600). 1804c8730
                // updates its split offset and sets the near depth to -.1.
                const int slot = static_cast<int>(i) + 1;
                const int cropStart = ((fx ? 426 : 21) - 135 * slot) / 4;
                const int cropEnd = (135 * slot - (fx ? 1060 : 655)) / 4;
                const float depth = fx ? .5f : 1.f;
                const Rect crop{-4.f * cropStart, 0, 768.f + 4 * (cropStart + cropEnd), 1500};
                const float x = (.5f + beamAnchors[i]) * laneWidth;
                const float radius = laneWidth * (fx ? .35f : .175f) * .5f;
                const float far = laneHeight * (1 - depth + laneBackDepth);
                render.afpOnPlane("beam", animation, label, elapsed, crop, {
                    projection.project(x - radius, far, half),
                    projection.project(x + radius, far, half),
                    projection.project(x - radius, laneBottom, half)
                }, false, fx ? .4f : 1.f);
            }
            for (auto bar = std::lower_bound(chart.barLines.begin(), chart.barLines.end(), drawTime);
                 bar != chart.barLines.end() && *bar <= visibleUntil; ++bar)
                drawLaneMeasure(render, projection, noteY(*bar), options.speed);
            return;
        }
        if (layer == PlayfieldLayer::FxHolds || layer == PlayfieldLayer::Notes) {
            const auto firstNote = std::lower_bound(noteTailMax.begin(), noteTailMax.end(), drawTime - 100) - noteTailMax.begin();
            const double judgeTime = stageJudgeTime(clock, options.judgmentOffset);
            const float noteHalfDepth = laneHeight * (scene.extendedNoteDepth() ? .012f : .008f);
            for (size_t i = firstNote; i < chart.notes.size(); ++i) {
                const auto& note = chart.notes[i];
                if (note.start > visibleUntil) break;
                if (note.judged || note.end < drawTime - 100) continue;
                const bool fx = note.button >= 4;
                if ((fx && note.longNote) != (layer == PlayfieldLayer::FxHolds)) continue;
                const float y = noteY(note.start);
                const float top = note.longNote ? noteY(note.end) : y - noteHalfDepth, bottom = y + noteHalfDepth;
                auto texture = fx ? "stage/notes_FX_chip.png" : "stage/notes_BT_chip.png";
                if (note.longNote) {
                    // 1804b08d0 alternates lp and onlp during contact, with separate offlp art after a miss or release.
                    const auto pulse = static_cast<int64_t>(now) / (fx ? 50 : 75);
                    if (note.active && (pulse & 1) == 0)
                        texture = fx ? "stage/notes_FX_long_onlp.png" : "stage/notes_BT_long_onlp.png";
                    else if (!note.active && judgeTime >= note.start)
                        texture = fx ? "stage/notes_FX_long_offlp.png" : "stage/notes_BT_long_offlp.png";
                    else
                        texture = fx ? "stage/notes_FX_long_lp.png" : "stage/notes_BT_long_lp.png";
                }
                drawLaneNote(render, projection, note.button, texture, top, bottom);
            }
            return;
        }
        // LaneView registers FX ground AFP at local p37 and BT ground AFP at p35.
        {
            for (size_t i = 0; i < holdEffects.size(); ++i) {
                const auto& hold = holdEffects[i];
                const bool held = hold.started >= 0;
                const double started = held ? hold.started : hold.released;
                if (started < 0) continue;
                const bool fx = i >= 4;
                if (fx != (layer == PlayfieldLayer::FxHoldEffects)) continue;
                const auto animation = fx ? "ef_FX_long" : "ef_BT_long";
                const auto intro = render.afpDuration("button_hit", animation, "FX_in");
                const double elapsed = now - started;
                const bool loop = held && elapsed >= intro;
                const auto clip = loop ? "FX_loop" : held ? "FX_in" : "FX_out";
                const double time = loop ? elapsed - intro : elapsed;
                if (!held && time >= render.afpDuration("button_hit", animation, clip)) continue;

                // 1804c5fb0 / 1804c8730 give the held effects their own XZ quads across the judge line.
                const float x = (.5f + beamAnchors[i]) * laneWidth;
                const float radius = laneWidth * (fx ? .35f : .145f) * .5f;
                const float origin = fx ? -.09f : -.045f;
                const float depth = fx ? .30f : .15f;
                const float near = laneHeight * (1 - origin);
                const float far = laneHeight * (1 - origin - depth);
                const int half = i < 2 || i == 4 ? -1 : 1;
                const auto size = render.afpSize("button_hit", animation, clip);
                render.afpOnPlane("button_hit", animation, clip, time, {0, 0, size.x, size.y}, {
                    projection.project(x - radius, far, half),
                    projection.project(x + radius, far, half),
                    projection.project(x - radius, near, half)
                }, loop);
            }
        }
    }

    Color color(int side) const {
        const auto index = side == 0 ? options.leftColor : options.rightColor;
        return index == side ? White : laserColors[std::clamp(index, 0, 3)];
    }

    void drawConsole(double now, const Projection& projection, double drawTime, ConsoleLayer layer) {
        const auto c = projection.cosine, s = projection.sine;
        constexpr Rect controls{-60, consoleY + 150, 1200, 600};
        std::array<double, 2> noticeElapsed{-1, -1};
        const bool playing = phase != Phase::Ready;
        const bool drawControls = layer == ConsoleLayer::ButtonsAndKnobs;
        const bool drawLaserHits = layer == ConsoleLayer::LaserHits;
        if (playing && (drawControls || drawLaserHits)) {
            const auto first = std::lower_bound(chart.laserStarts.begin(), chart.laserStarts.end(), drawTime,
                [](const auto& notice, double time) { return notice.second < time; });
            const double lead = drawTime + laserNoticeLeadMs;
            for (auto it = first; it != chart.laserStarts.end() && it->second <= lead; ++it) {
                const auto [side, time] = *it;
                if (noticeElapsed[side] < 0) noticeElapsed[side] = lead - time;
            }
        }
        render.pushTransform({c,-s,Width*.5f*(1-c)+rotationY*s,
                              s,c,rotationY*(1-c)-Width*.5f*s});
        constexpr Rect consoleBounds{-360, consoleY, 1800, 900};
        const double elapsed = lastFrame < 0 ? 0 : now - startedAt;
        if (layer == ConsoleLayer::JudgeLine)
            render.afp("game_nemsys", "hantei_line", "0", elapsed, false, consoleBounds, console->art);
        if (layer == ConsoleLayer::Body)
            render.afp("game_nemsys", "io", "io_in", 0, false, consoleBounds, console->art);
        if (drawControls) {
            for (int i = 0; i < 6; ++i) {
                const auto& animation = console->buttons[i];
                if (hitAt[i] >= 0 && now - hitAt[i] < render.afpDuration("stage_buttons", animation, "btn_hit")) {
                    render.afp("stage_buttons", animation, "btn_hit", now - hitAt[i], false, controls);
                } else if (buttons[i] || (options.autoPlay && chart.holding(i))) {
                    render.afp("stage_buttons", animation, "btn_push", 0, false, controls);
                }
            }
        }
        if (playing && drawControls) {
            // The NEMSYS view (18043a570, 18043b230) has its own two AFPs.
            // Keep the control light in console space as the laser moves.
            for (int side = 0; side < 2; ++side) {
                const auto& fx = laserFx[side];
                const auto& animation = console->knobs[side];
                const double start = fx.heldAt >= 0 ? fx.heldAt : fx.turnedAt;
                const double elapsed = now - start;
                const double enter = render.afpDuration("stage_buttons", animation, "vol_hit_in");
                if (start >= 0 && elapsed < enter) {
                    render.afp("stage_buttons", animation, "vol_hit_in", elapsed, false, controls);
                } else if (fx.heldAt >= 0) {
                    render.afp("stage_buttons", animation, "vol_loop", elapsed - enter, true, controls);
                } else if (noticeElapsed[side] >= 0) {
                    render.afp("stage_buttons", animation, "vol_notice", noticeElapsed[side], true, controls);
                }
            }
        }
        if (playing && layer == ConsoleLayer::ButtonHits) {
            const Projection flat{projection.pitch, projection.radius, projection.shift, 1, 0};
            const auto in = options.sCritical ? "btnS_in" : "btn_in";
            const auto loop = options.sCritical ? "btnS_loop" : "btn_loop";
            const auto out = options.sCritical ? "btnS_out" : "btn_out";
            const auto inMs = render.afpDuration("button_hit", "ef_btn_all", in);
            for (int i = 0; i < 6; ++i) {
                const auto& hold = holdEffects[i];
                auto clip = buttonHitLabel(chart.feedback(i), options.sCritical);
                double started = feedbackAt[i];
                bool looping = false;
                if (hold.started >= 0) {
                    looping = now - hold.started >= inMs;
                    clip = looping ? loop : in;
                    started = hold.started + (looping ? inMs : 0);
                } else if (hold.released >= 0 && hold.released >= started) {
                    clip = out;
                    started = hold.released;
                }
                const double elapsed = now - started;
                if (started < 0 || (!looping && elapsed >= render.afpDuration("button_hit", "ef_btn_all", clip))) continue;
                const auto anchor = flat.point((.5f + hitAnchors[i]) * laneWidth, laneHeight, hitAnchors[i] < 0 ? -1 : 1);
                render.afp("button_hit", "ef_btn_all", clip, elapsed, looping,
                           {anchor.x - 360, anchor.y - 200, 720, 400});
            }
        }
        render.popTransform();
        if (!playing) return;
        if (drawLaserHits || layer == ConsoleLayer::LaserCursor) {
            const double judgmentTime = (options.autoPlay ? clock : stageJudgeTime(clock, options.judgmentOffset) - 12)
                + (phase == Phase::Ending ? now - phaseAt : 0);
            for (int side = 0; side < 2; ++side) {
                const auto cursor = chart.cursorForDraw(side, drawTime);
                const auto& fx = laserFx[side];
                const auto effect = [&](std::string_view name, std::string_view clip, double elapsed, float at, bool loop) {
                    if (elapsed < 0 || (!loop && elapsed >= render.afpDuration("laser_effect", name, clip))) return;
                    const auto anchor = projection.point(laserX(at), laneHeight, 0);
                    const auto size = render.afpSize("laser_effect", name, clip);
                    constexpr float scale = .65f;
                    render.afp("laser_effect", name, clip, elapsed, loop,
                        {anchor.x - size.x * scale / 2, anchor.y - size.y * scale / 2, size.x * scale, size.y * scale});
                };
                const auto hold = laserArt[side].holdEffect;
                if (drawLaserHits && cursor && fx.heldAt >= 0)
                    effect(hold, hold.starts_with("ef_vol_") ? "loop" : "0", now - fx.heldAt, *cursor, true);
                const auto& exit = chart.laserExit(side);
                if (drawLaserHits && exit.time >= 0)
                    effect(laserReleaseEffects[side], "0", judgmentTime - exit.time, exit.position, false);
                const auto& corner = chart.laserCorner(side);
                if (drawLaserHits && corner.time >= 0) {
                    const auto name = laserCornerEffects[side][corner.direction + 1];
                    const double elapsed = judgmentTime - corner.time;
                    if (elapsed >= 0 && elapsed < render.afpDuration("laser_effect", name, "0")) {
                        const auto anchor = projection.point(laserX(corner.position), laneHeight, 0);
                        const auto size = render.afpSize("laser_effect", name, "0");
                        const float x = anchor.x - c * size.x / 2 + s * size.y / 2;
                        const float y = anchor.y - s * size.x / 2 - c * size.y / 2;
                        render.afpOnPlane("laser_effect", name, "0", elapsed, {0, 0, size.x, size.y},
                            {{{x, y, 1}, {x + c * size.x, y + s * size.x, 1}, {x - s * size.y, y + c * size.y, 1}}});
                    }
                }
                if (drawLaserHits && fx.turn) {
                    // GameJudgeTumamiBombView dispatches these HG layers together.
                    // 180435710 supplies their pivots. 180436bb0 moves the curve
                    // and dust over 0.4 s using the two original interpolators.
                    constexpr std::array<Vec2, 3> pivots{{{215,520}, {750,920}, {214,522}}};
                    const double elapsed = std::max(0., now - fx.turnedAt);
                    const float phase = static_cast<float>(std::min(elapsed / 400., 1.));
                    const float radians = phase * std::numbers::pi_v<float> / 2;
                    const float start = fx.turn->start.position;
                    const float direction = static_cast<float>(fx.turn->direction());
                    const std::array positions{
                        start + direction * 2 * fx.turn->range() * std::sin(radians),
                        .5f,
                        std::lerp(start, fx.turn->end.position, 1 - std::cos(radians))
                    };
                    const float flip = -direction;
                    for (size_t index = 0; index < pivots.size(); ++index) {
                        const auto name = laserArt[side].slamEffects[index];
                        if (elapsed >= render.afpDuration("laser_effect", name, "0")) continue;
                        const auto anchor = projection.point(laserX(positions[index]), laneHeight, 0);
                        const auto size = render.afpSize("laser_effect", name, "0");
                        const auto corner = [&](float x, float y) {
                            x = (x - pivots[index].x) * flip;
                            y -= pivots[index].y;
                            return Render::Projected{anchor.x + c*x - s*y, anchor.y + s*x + c*y, 1};
                        };
                        render.afpOnPlane("laser_effect", name, "0", elapsed, {0, 0, size.x, size.y},
                                          {corner(0, 0), corner(size.x, 0), corner(0, size.y)});
                    }
                }
                if (!cursor || drawLaserHits) continue;
                const auto p = projection.point(laserX(*cursor), laneHeight, 0);
                // Event 7 toggles visibility. The AFP instance advances while hidden.
                if (fx.heldAt >= 0) {
                    const auto name = laserCriticalEffects[side];
                    const auto size = render.afpSize("laser_effect", name, "0");
                    const float x = p.x - c * size.x / 2 + s * size.y / 2;
                    const float y = p.y - s * size.x / 2 - c * size.y / 2;
                    render.afpOnPlane("laser_effect", name, "0", now - startedAt, {0, 0, size.x, size.y},
                        {{{x, y, 1}, {x + c * size.x, y + s * size.x, 1}, {x - s * size.y, y + c * size.y, 1}}}, true);
                }
                const auto size = render.imageSize(cursorTexture[side]);
                const auto width = 76 * size.x / std::max(1.f, size.y);
                render.image(cursorTexture[side], {p.x - width / 2, p.y - 52, width, 76}, color(side));
            }
        }
        if (!drawLaserHits) return;
        for (int side = 0; side < 2; ++side) {
            const double elapsed = noticeElapsed[side];
            if (elapsed < 0) continue;
            const auto duration = render.afpDuration("laser_notice", notices[side], "0");
            render.afp("laser_notice", notices[side], "0", std::min(elapsed, std::max(0.0, duration - .01)));
        }
    }

    void drawTouchButtons() {
        constexpr float textureSize = 1254;
        constexpr Rect notePixels{134, 131, 987, 991}, fxPixels{32, 456, 1191, 353};
        // Keep the approved FX face and clip the residual checkerboard outside its outline.
        constexpr std::array fxOutline{Vec2{104, 458}, Vec2{1220, 458}, Vec2{1220, 744},
                                       Vec2{1157, 807}, Vec2{33, 807}, Vec2{33, 528}};
        for (int button = BTA; button <= FXR; ++button) {
            const bool fx = button >= FXL;
            const bool mirrored = fx ? button == FXR : button < BTC;
            const bool pressed = buttons[button];
            const float inset = pressed ? 8.f : 4.f;
            const auto key = touchKeys[button];
            const Rect face{key.x + inset, key.y + inset, key.w - 2 * inset, key.h - 2 * inset};
            const auto pixels = fx ? fxPixels : notePixels;
            const Rect uv{(pixels.x + (mirrored ? pixels.w : 0.f)) / textureSize, pixels.y / textureSize,
                          (mirrored ? -pixels.w : pixels.w) / textureSize, pixels.h / textureSize};
            const Color tint = pressed ? Color{.8f, .8f, .8f} : White;
            const float right = face.x + face.w, bottom = face.y + face.h;
            if (!fx) {
                render.quad(touchNoteFace, {Vec2{face.x, face.y}, Vec2{right, face.y},
                                           Vec2{right, bottom}, Vec2{face.x, bottom}},
                            tint, Transparent, Blend::Alpha, uv);
                continue;
            }
            auto outline = fxOutline;
            for (auto& point : outline) {
                const float x = (point.x - pixels.x) / pixels.w;
                point = {face.x + (mirrored ? 1.f - x : x) * face.w,
                         face.y + (point.y - pixels.y) / pixels.h * face.h};
            }
            render.projectedStrip(touchFxFace, {Render::Projected{face.x, face.y, 1}, Render::Projected{right, face.y, 1},
                                               Render::Projected{face.x, bottom, 1}, Render::Projected{right, bottom, 1}},
                                  uv, tint, Blend::Alpha, false, outline);
        }
    }

    void drawHud(double now) {
        if (startHeld) hudBindings.erase("pt_sousa");
        else hudBindings["pt_sousa"] = "";
        const auto& stats = chart.stats();
        int score = options.exScore ? chart.exScore() : chart.score();
        for (int digit = 0; digit < 8; ++digit) {
            auto& binding = hudBindings["score_" + std::string(digit + 1, '0') + "_usr"];
            binding.path = "hud/num_score_" + std::to_string(score % 10) + ".png";
            binding.tint = digit > 0 && score == 0 ? ScoreLeadingZero : White;
            score /= 10;
        }
        const bool permissive = options.permissiveRate >= 0;
        if (permissive) {
            hudBindings["gauge_effective"] = "hud/gauge_permissiv.png";
        } else if (stats.ultra) {
            hudBindings["gauge_effective"] = options.ars ? "hud/gauge_ultra_ars.png" : "hud/gauge_ultra.png";
        } else if (stats.excessive) {
            hudBindings["gauge_effective"] = "cabinet/gauge_excessive.png";
        } else {
            hudBindings.erase("gauge_effective");
        }
        char chain[16];
        std::snprintf(chain, sizeof(chain), "%04d", stats.maxChain);
        char speed[16];
        std::snprintf(speed, sizeof(speed), "%.2f", options.speed);
        const auto left = static_cast<int>(std::ceil(std::max(0.0, options.practiceUntil > 0
            ? options.practiceUntil - now : 300000 - clock) / 1000));
        char timer[16];
        std::snprintf(timer, sizeof(timer), "%02d:%02d", left / 60, left % 60);
        // Offline play uses NABLA hud_main. Its FRAME labels retain the authored layout.
        render.afp("hud", "hud_main", "hud_loop", 0, false, {0, 0, Width, Height}, hudBindings);
        // 18042f648 seeks track_time_usr by truncating progress * music_end.
        const int progressFrame = static_cast<int>(std::clamp(clock / chart.endMs, 0.0, 1.0) * 1000);
        render.afp("hud", "hud_time", "music_start", progressFrame * 1000.0 / 120 + .01,
                   false, {210, 262, 660, 30});
        render.text(chain, 845, 488, 23, White, TextAlign::Center, 90, TextFont::Display);
        render.text(std::to_string(int(chart.bpmAt(clock))), 199, 479, 24, White, TextAlign::Center, 70, TextFont::Display);
        render.text(speed, 141, 528, 24, White, TextAlign::Center, 70, TextFont::Display);
        render.text(timer, 172, 697, 23, White, TextAlign::Center, 88, TextFont::Display);
        if (options.practiceUntil > 0) render.text(options.autoRetry ? "ON" : "OFF", 984, 1857, 17);
        if (options.autoPlay) render.text("AUTO", 933, 1857, 20, White, TextAlign::Right, 85, TextFont::Display);
        if (options.exScore) render.text("EX SCORE", 790, 369, 16);
    }

    void drawHudTrack() {
        const auto& stats = chart.stats();
        const double gauge = std::clamp(stats.gauge, 0., 1.);
        const int gaugeFrame = static_cast<int>(std::lround(gauge * 500));
        const auto gaugeAnimation = options.permissiveRate >= 0 ? "gauge_permissive_stagemain"
            : stats.ultra ? "gauge_ultra_stagemain"
            : stats.excessive ? "gauge_excessive_stagemain" : "gauge_effective_stagemain";
        char gaugeText[16];
        std::snprintf(gaugeText, sizeof(gaugeText), "%.1f", gauge * 100.);
        render.afp("hud", gaugeAnimation, "0_per", gaugeFrame * 1000.0 / 120 + .01,
            false, {860, 578, 160, 530}, gaugeBindings);
        render.text(gaugeText, 910, 1100 - gaugeFrame, 17, White, TextAlign::Right, 46, TextFont::Display);
        render.text(title, 382, 285, 21, White, TextAlign::Left, 450, TextFont::MaruGothicW4);
        // HudTrack registers level_usr and title as separate p39 layers.
        drawHorizontalLevel(render, selection, {13, 385, 200, 50});
        // HudStagemainBase binds this original child at 75% on p39.
        render.afp("hud", "small_1_stagemain", "0", 0, false, {-46,729.5f,450,375});
        drawProfileCard(render, {10.25f,736.25f,225,225}, player);
        drawHorizontalLevel(render, selection, {9.5f,958.06f,103.5f,25.875f});
        if (previousBest > 0) number(render, previousBest, 8, 240, 963, 18, "num_score_small");
    }

    void drawCombo(double now, bool milestone) {
        const auto& stats = chart.stats();
        const auto bindChain = [&](int value, bool gold, bool dimLeadingZeros) {
            for (int digit = 0; digit < 4; ++digit) {
                auto& binding = chainBindings["chain_" + std::string(digit + 1, '0') + "_usr"];
                binding.path = (gold ? "hud/num_chain_a_" : "hud/num_chain_b_") + std::to_string(value % 10) + ".png";
                binding.sourceRect = {1, 1, 54, 60};
                binding.tint.a = dimLeadingZeros && digit > 0 && value == 0 ? .3f : 1.f;
                value /= 10;
            }
        };
        if (milestone && chainMilestone > 0) {
            // 180434575–18043487c dispatches 01–05 and then returns to 02.
            const auto clip = chainMilestoneClips[chainMilestone == 1 ? 0 : 1 + (chainMilestone - 2) % 4];
            const double elapsed = now - chainMilestoneAt;
            if (elapsed < render.afpDuration("hud_chain", "eff_chain", clip)) {
                bindChain(chainMilestone * 100, chainMilestoneGold, false);
                render.afp("hud_chain", "eff_chain", clip, elapsed, false, {0, 0, Width, Height}, chainBindings);
            }
        }
        if (!milestone && stats.chain > 0) {
            bindChain(stats.chain, stats.errors == 0, true);
            render.afp("hud_chain", "chain", "0", 0, false, {0, 0, Width, Height}, chainBindings);
        }
    }

    void drawJudgments(double now) {
        if (options.timingPosition == 4) return;
        // GameJudgeComboView 180433c70: two fixed columns, or a single centered column.
        constexpr std::array<float, 4> positionOffset{0, 228, -489, -264};
        const float baseline = 1063 + positionOffset[options.timingPosition];
        const bool both = options.cueStyle != 0 && options.timingMsStyle != 0;
        for (size_t i = 0; i < timingCues.size(); ++i) {
            const auto& cue = timingCues[i];
            if (cue.at < 0 || now - cue.at >= 750) continue;
            const bool early = cue.result < judgmentCritical;
            const bool near = cue.result == 2 || cue.result == 8;
            const float alpha = (static_cast<int64_t>(now) % 125 < 62 ? 1.f : .85f) * (160.f / 255);
            Color tint{1, 1, 1, alpha};
            Color outline{.667f, .667f, .667f, alpha};
            if (options.timingColor) {
                outline = early ? Color{1, .4f, .6f, alpha} : Color{.4f, .6f, 1, alpha};
                tint = near ? (early ? Color{1, .4f, .4f, alpha} : outline)
                            : (early ? Color{1, .8f, .8f, alpha} : Color{.8f, 1, 1, alpha});
            }
            char offset[24];
            if (i == 1) std::snprintf(offset, sizeof(offset), "%c%.1fms", early ? '+' : '-', std::abs(cue.offsetMs));
            const std::string_view text = i == 1 ? offset : early ? "EARLY" : "LATE";
            render.text(text, both ? (i == 0 ? 450.f : 630.f) : 540.f, baseline, 26, tint,
                        TextAlign::Center, 0, TextFont::Display, 3, outline);
        }
    }
};

void LanePreview::update(double positionMs) {
    if (previousPosition - positionMs > previewLoopMs / 2) ++cycles;
    previousPosition = positionMs;
    timeMs = cycles * previewLoopMs + positionMs;
}

void LanePreview::press(int button, double now, double inputAt, const PlayOptions& options) {
    const double time = stageJudgeTime(timeMs - std::max(0.0, now - inputAt), options.judgmentOffset);
    const int beat = static_cast<int>(std::lround((time / previewBeatMs - button) / 4)) * 4 + button;
    if (beat < 0 || hits[button].beat == beat) return;
    const double offset = beat * previewBeatMs - time;
    const float delta = static_cast<float>(-offset / 1000);
    // The 180384550 generator feeds the original BT chip class (1804b2240).
    constexpr std::array<std::pair<float, int>, 8> windows{{
        {-.2333333343f, 1}, {-.1333333254f, 2}, {-.0416666642f, 3}, {-.0208333321f, 4},
        {0, 6}, {.0208333358f, 7}, {.0416666679f, 8}, {.1333333403f, 10}
    }};
    int judgment = 0;
    for (const auto& [from, result] : windows) {
        if (delta < from) break;
        judgment = result;
    }
    if (judgment > 0 && judgment != judgmentMiss) hits[button] = {beat, judgment, inputAt, offset};
}

void LanePreview::draw(Render& render, double now, const PlayOptions& options,
                       const std::array<bool, ButtonCount>& held) const {
    const Projection projection{defaultPitch, defaultRadius, 0, 1, 0};
    drawLaneSurface(render, projection, White, {0, 0, 0, 1});
    const double time = stageDrawTime(timeMs, options.displayOffset);
    const double span = 42500. / options.speed;
    const auto depth = [=](int beat) { return static_cast<float>((1 - (beat * previewBeatMs - time) / span) * laneHeight); };
    const int first = std::max(0, static_cast<int>(std::floor(time / previewBeatMs)));
    const int last = static_cast<int>(std::ceil((time + span) / previewBeatMs));
    // 180384550 generates one BT object per beat, in A–D order, and a line every four beats.
    for (int beat = first; beat <= last; ++beat) {
        const float y = depth(beat);
        const int button = beat % 4;
        if (hits[button].beat != beat)
            drawLaneNote(render, projection, button, "stage/notes_BT_chip.png", y - 13, y + 13);
        if (button == 0 && y >= 0 && y <= laneHeight) drawLaneMeasure(render, projection, y, options.speed);
    }
    const auto& skin = nemsysSkin(options.nemsys);
    Bindings art{{"io_main_i_eeb", skin.image}};
    const std::string suffix = skin.io == "v" ? "" : "_" + skin.io;
    if (!suffix.empty()) {
        art.emplace("io_front_v", "game_nemsys/io_front" + suffix + ".png");
        art.emplace("hantei_line_v", "game_nemsys/hantei_line_" + (skin.io == "i" ? "ii" : skin.io) + ".png");
    }
    constexpr Rect console{-360, consoleY, 1800, 900};
    render.afp("game_nemsys", "hantei_line", "0", 0, false, console, art);
    render.afp("game_nemsys", "io", "io_in", 0, false, console, art);
    const Hit* recent = nullptr;
    for (int button = BTA; button <= BTD; ++button) {
        const auto& hit = hits[button];
        const double elapsed = now - hit.at;
        if (hit.at >= 0 && elapsed < render.afpDuration("stage_buttons", std::string(lights[button]) + suffix, "btn_hit"))
            render.afp("stage_buttons", std::string(lights[button]) + suffix, "btn_hit", elapsed, false,
                       {-60, consoleY + 150, 1200, 600});
        else if (held[button])
            render.afp("stage_buttons", std::string(lights[button]) + suffix, "btn_push", 0, false,
                       {-60, consoleY + 150, 1200, 600});
        const auto clip = buttonHitLabel(hit.judgment, options.sCritical);
        if (hit.at >= 0 && elapsed < render.afpDuration("button_hit", "ef_btn_all", clip)) {
            const auto anchor = projection.point((.5f + hitAnchors[button]) * laneWidth, laneHeight, button < 2 ? -1 : 1);
            render.afp("button_hit", "ef_btn_all", clip, elapsed, false, {anchor.x - 360, anchor.y - 200, 720, 400});
        }
        if (hit.at >= 0 && elapsed < 750 && (!recent || hit.at > recent->at)) recent = &hit;
    }
    if (recent && options.timingPosition != 4) {
        const int result = recent->judgment;
        const bool near = result == 2 || result == 8;
        const bool critical = result == 3 || result == 7;
        const auto shows = [&](int style) { return style == 3 || (style > 0 && near) || (style == 2 && critical); };
        const bool direction = shows(options.cueStyle), value = shows(options.timingMsStyle);
        const bool early = recent->offset > 0;
        constexpr std::array<float, 4> offsets{0, 228, -489, -264};
        const float y = 1063 + offsets[options.timingPosition];
        const Color color = options.timingColor ? (early ? Color{1, .4f, .6f, 1} : Color{.4f, .6f, 1, 1}) : White;
        if (direction) render.text(early ? "EARLY" : "LATE", value ? 450 : 540, y, 26, color,
                                   TextAlign::Center, 0, TextFont::Display);
        if (value) {
            char text[24];
            std::snprintf(text, sizeof(text), "%+.1fms", recent->offset);
            render.text(text, direction ? 630 : 540, y, 26, color, TextAlign::Center, 0, TextFont::Display);
        }
    }
}

Stage::Stage(Render& render, Audio& audio, Selection selection, PlayOptions options, PlayerCard player,
             int previousBest, std::shared_ptr<const SongData> song, bool deferTextures)
    : impl_(std::make_unique<Impl>(render, audio, std::move(selection), options, std::move(player),
                                  previousBest, std::move(song), deferTextures)) {
    if (!deferTextures) prepareAudio();
}

void Stage::prepareAudio() {
    auto& s = *impl_;
    if (!s.audio.prepareMusicData(s.selection.asset(s.selection.music), s.song->music))
        throw std::runtime_error("Unable to prepare stage music");
    s.audio.setDsp([](void* context, float* samples, int frames, int64_t first) {
        static_cast<Stage*>(context)->processAudio(samples, frames, first);
    }, this);
    s.musicPrepared = true;
}

Stage::~Stage() {
    if (impl_->deferTextures) impl_->render.releaseStageImages();
    if (!impl_->musicPrepared) return;
    impl_->audio.setDsp();
    impl_->audio.stopMusic();
}

bool Stage::prepareGraphics() {
    const bool sceneReady = impl_->scene.prepareGraphics();
    if (!sceneReady || !impl_->render.stageImagesReady())
        return false;
    if (!impl_->musicPrepared) prepareAudio();
    return true;
}

void Stage::processAudio(float* samples, int frames, int64_t firstFrame) {
    impl_->chart.processAudio(samples, frames, firstFrame, impl_->song->dryPcm);
}

void Stage::releaseGraphics() {
    impl_->scene.releaseGraphics();
}

void Stage::restoreGraphics() {
    if (impl_->options.valkyrieEffect && !impl_->chart.lasers.empty()) impl_->render.prepareLaserGlow();
    impl_->scene.restoreGraphics();
    impl_->queueSceneGraphics();
}

void Stage::update(double nowMs) {
    auto& s = *impl_;
    if (s.phase == Impl::Phase::Complete) return;
    if (s.phaseAt < 0) {
        s.startedAt = s.phaseAt = nowMs;
        SDL_Log("SDVX READY %s",s.selection.title.c_str());
    }
    // Original login capture: matching_ready2 -> +1.1516 s main_ready.
    // Keep the complete sample; its tail continues over the first music bars.
    if (s.phase == Impl::Phase::Ready && !s.readySound && nowMs - s.phaseAt >= 1150) {
        s.readySound = true;
        s.audio.sfx("stage_ready/ready.wav");
    }
    if (s.phase == Impl::Phase::Ready && nowMs - s.phaseAt >= s.readyDuration) {
        s.audio.startMusic();
        s.phase = Impl::Phase::Playing;
        s.phaseAt = nowMs;
        s.buttons.fill(false);
        s.physicalButtons.fill(false);
        SDL_Log("SDVX Stage started %s %s", s.selection.title.c_str(), s.selection.difficulty.c_str());
    }
    if (s.phase == Impl::Phase::Playing) {
        if (s.postRollAt < 0 && s.audio.musicEnded()) s.postRollAt = nowMs;
        s.clock = s.chartTime(nowMs);
        s.frozen = s.clock;
        const auto judgment = s.options.autoPlay ? s.clock : stageJudgeTime(s.clock,s.options.judgmentOffset);
        s.chart.advance(judgment - (s.options.autoPlay ? 0 : 12));
        if (s.options.autoPlay) {
            const auto& feedback = s.chart.autoplayFeedback();
            for (const auto* note : feedback.samples) s.queueButtonSound(note->sample, note->start);
            for (const auto* slam : feedback.slams) s.hitSlam(*slam, s.clock, nowMs);
        }
        // GameAudio 1804b7eb0 queues FX chips at the authored music position.
        // 180644460 seeks ordinary samples by their lateness. Slots 11–13 restart.
        while (!s.buttonSounds.empty() && s.buttonSounds.front().due <= s.clock) {
            const auto [due, slot] = s.buttonSounds.front();
            const double offset = slot >= 11 && slot <= 13 ? 0 : s.clock - due;
            s.audio.sfx("stage/sound/" + std::to_string(slot) + ".wav", s.fxSampleGains[slot - firstFxSample], 1, offset);
            s.buttonSounds.pop_front();
        }
        while (s.nextSceneSound < s.sceneSounds.size() && s.sceneSounds[s.nextSceneSound].due <= s.clock) {
            const auto& sound = s.sceneSounds[s.nextSceneSound++];
            s.audio.sfx(sound.path, sound.gain, 1);
        }
        while (s.nextGauge < s.gaugeHistory.size() && s.clock >= s.chart.endMs * s.nextGauge / 99.0) {
            s.gaugeHistory[s.nextGauge++] = s.chart.stats().gauge;
        }
        const auto& stats = s.chart.stats();
        bool challengeFailed = false;
        if (s.options.practiceUntil > 0 && s.options.challenge != 0) {
            // PREMIUM TIME crashes as soon as its target is unreachable.
            // The matching-opponent target has no score in an offline session.
            const std::array targets{0, s.previousBest, 9900000, 9800000, 9700000};
            const auto possible = (2LL * (stats.total - stats.errors) - stats.near) * 5000000 / std::max(1, stats.total);
            if (s.options.challenge == 1) challengeFailed = possible < targets[s.options.target];
            else if (s.options.challenge == 2) challengeFailed = stats.errors != 0 || stats.near != 0;
            else if (s.options.challenge == 3) challengeFailed = stats.errors != 0;
        }
        if (s.chart.failedRate() || challengeFailed || judgment >= s.chart.endMs + 180) {
            s.chart.advance(judgment);
            s.finish(nowMs, challengeFailed);
        }
    }
    // 1804b8f84 consumes the sound queue in order, after its due millisecond.
    const double audioFrame = std::floor(nowMs);
    while (!s.slamSoundDue.empty() && s.slamSoundDue.front() < audioFrame) {
        // Both knobs restart the same bank 13, slot 0 voice (180733090).
        s.audio.sfx("stage/sound/virtical_shot.wav", slamSampleGain, 1);
        s.slamSoundDue.pop_front();
    }
    if (s.phase == Impl::Phase::Ending) {
        // 1804d2e81–1804d2eaf measures the held chord during the crash sequence.
        if (s.shortcutAt >= 0 && nowMs - s.shortcutAt > PremiumRetryHoldMs && nowMs < s.options.practiceUntil) {
            s.quickRetry();
            return;
        }
        if (nowMs - s.phaseAt >= s.endDuration) s.makeResult(nowMs);
    }
    const double drawTime = stageDrawTime(s.clock,s.options.displayOffset);
    const auto& gauge = s.chart.stats();
    s.scene.setGauge(drawTime, !gauge.excessive && s.options.permissiveRate < 0 && gauge.gauge >= .7);
    while (s.nextCue < s.chart.cues.size() && s.chart.cues[s.nextCue].time <= drawTime) {
        s.applyCue(s.chart.cues[s.nextCue++]);
    }
    s.scene.update(drawTime, nowMs, s.phase == Impl::Phase::Ready ? 0 : s.chart.bpmAt(s.clock));
    s.syncJudgmentFeedback(nowMs);
    for (int side = 0; side < 2; ++side) {
        auto& fx = s.laserFx[side];
        if (s.chart.tracking(side)) {
            if (fx.heldAt < 0) fx.heldAt = nowMs;
        } else fx.heldAt = -1;
    }
    const float delta = s.lastFrame < 0 ? 0.f : static_cast<float>(std::floor(nowMs) - std::floor(s.lastFrame));
    // AngleUpdater 18044c335–18044c454. One accumulator unit is ten degrees.
    constexpr float neutral = 1.f / 127;
    const auto [target, speed] = s.chart.tiltTarget(drawTime);
    if (std::abs(target) >= neutral) {
        const float step = delta * .00375f * speed;
        s.roll += std::clamp(target - s.roll, -step, step);
    } else if (std::abs(s.roll) >= neutral) {
        const float step = std::min(delta * speed * .0009375f, std::abs(s.roll) / 15 + neutral);
        s.roll += std::copysign(step, target - s.roll);
    } else s.roll = 0;
    s.lastFrame = nowMs;
}

void Stage::draw(double now) {
    auto& s = *impl_;
    const double drawTime = stageDrawTime(s.clock,s.options.displayOffset);
    const auto pitch = s.pitch.at(drawTime), radius = s.radius.at(drawTime);
    constexpr float radians = std::numbers::pi_v<float> / 180;
    const float autoTilt = s.roll * 10 * radians;
    // BIL_RotZ dispatch (180425ab0) reverses the authored rotation in mirror mode.
    const float billboardAngle = s.billboardRotation.at(drawTime) * (s.options.mirror ? -1.f : 1.f);
    const float angle = autoTilt + billboardAngle;
    const Projection projection{
        pitch, radius, s.morph.at(drawTime) * .125f * 61, std::cos(angle), std::sin(angle)
    };
    s.drawOriginalStage(now, drawTime, projection, autoTilt, angle);
    if (s.touchControls && s.phase == Impl::Phase::Playing) s.drawTouchButtons();
}

void Stage::setTouchControls(bool enabled, double now) {
    auto& s = *impl_;
    s.stopInputs(stageJudgeTime(s.chartTime(now), s.options.judgmentOffset));
    s.touchControls = enabled && !s.options.autoPlay;
}

void Stage::input(const GameInput& event) {
    auto& s = *impl_;
    if (s.phase == Impl::Phase::Complete) return;
    using Type = GameInput::Type;
    const double now = applicationTime();
    const double inputAt = std::min(now, event.time);
    const double musicTime = s.chartTime(now);
    const double time = stageJudgeTime(musicTime - (now - inputAt), s.options.judgmentOffset);
    if (event.type == GameInput::Type::Back || (event.type == GameInput::Type::KeyUp && event.button == Back)) {
        s.audio.pauseMusic();
        s.result = Result{};
        s.result->cancelled = true;
        s.phase = Impl::Phase::Complete;
        return;
    }
    if (event.type == GameInput::Type::FocusLost) {
        s.stopInputs(time);
        return;
    }
    const auto turn = [&](int side, float delta) {
        if (const auto* slam = s.chart.rotate(side, delta, time)) s.hitSlam(*slam, musicTime, now);
    };
    if (event.type == GameInput::Type::Rotary || event.type == GameInput::Type::Scroll) {
        const std::array<float, 2> delta{event.left, event.right};
        for (int side = 0; side < 2; ++side) {
            if (s.startHeld) {
                s.speedRemainder[side] += delta[side];
                const auto step = static_cast<int>(s.speedRemainder[side] / 20);
                s.speedRemainder[side] -= step * 20;
                s.options.speed = std::clamp(s.options.speed + step * .05, .5, 12.0);
            } else if (s.phase == Impl::Phase::Playing && !s.options.autoPlay) {
                turn(side, delta[side]);
            }
        }
        return;
    }
    bool down = event.type == Type::KeyDown;
    int button = event.button;
    const bool touch = event.type == Type::TouchDown || event.type == Type::TouchMove ||
                       event.type == Type::TouchUp || event.type == Type::TouchCancel;
    if (touch) {
        if (!s.touchControls || s.phase != Impl::Phase::Playing) return;
        down = event.type == Type::TouchDown;
        if (down) {
            const auto key = std::ranges::find_if(touchKeys, [&](Rect bounds) {
                return event.x >= bounds.x && event.x < bounds.x + bounds.w &&
                       event.y >= bounds.y && event.y < bounds.y + bounds.h;
            });
            if (key != touchKeys.end()) {
                button = static_cast<int>(key - touchKeys.begin());
                if (s.touchButtons[button]) return;
                s.touchButtons[button] = event.finger;
            } else {
                if (event.x < 0 || event.x >= Width || event.y < 0 || event.y >= touchKeysY) return;
                auto& dial = s.touchDials[event.x >= Width / 2];
                if (!dial) dial = Impl::TouchDial{event.finger, event.x};
                return;
            }
        } else {
            // Each finger keeps the key or laser side where it started, even across the center.
            for (int side = 0; side < 2; ++side) {
                auto& dial = s.touchDials[side];
                if (!dial || dial->finger != event.finger) continue;
                if (event.type == Type::TouchMove) {
                    constexpr float unitsPerPixel = StageChart::RotaryUnitsPerLane / (Width / 2);
                    const float dx = event.x - std::exchange(dial->x, event.x);
                    turn(side, dx * unitsPerPixel);
                } else dial.reset();
                return;
            }
            if (event.type == Type::TouchMove) return;
            const auto key = std::ranges::find(s.touchButtons, std::optional{event.finger});
            if (key == s.touchButtons.end()) return;
            button = static_cast<int>(key - s.touchButtons.begin());
            key->reset();
        }
    } else if ((!down && event.type != Type::KeyUp) || event.repeat) return;
    if (down && button < 0) {
        // 180472250 reads the cabinet's keypad rows 2/8 (sound) and 3/9 (draw).
        switch (event.key) {
        case SDL_SCANCODE_KP_2: s.options.setTimingValue(0, s.options.timingValue(0) - 1); return;
        case SDL_SCANCODE_KP_8: s.options.setTimingValue(0, s.options.timingValue(0) + 1); return;
        case SDL_SCANCODE_KP_3: s.options.setTimingValue(1, s.options.timingValue(1) - 1); return;
        case SDL_SCANCODE_KP_9: s.options.setTimingValue(1, s.options.timingValue(1) + 1); return;
        default: break;
        }
    }
    if (button < 0 && (event.key == SDL_SCANCODE_KP_00 || event.key == SDL_SCANCODE_KP_0) && down
        && s.phase == Impl::Phase::Playing && now < s.options.practiceUntil) {
        s.options.autoRetry = !s.options.autoRetry;
        return;
    }
    if (button == Start) {
        s.startHeld = down;
        if (!down) s.speedRemainder.fill(0);
    } else if (button >= BTA && button <= FXR) {
        if (!touch) s.physicalButtons[button] = down;
        s.setButton(button, s.physicalButtons[button] || s.touchButtons[button].has_value(), time, inputAt, now);
    }
    if (now >= s.options.practiceUntil) {
        s.shortcutAt = -1;
        return;
    }
    const bool chord = s.startHeld && s.buttons[FXL] && s.buttons[FXR];
    if (chord && s.phase == Impl::Phase::Playing) {
        // GameInputCheckRequestForceTrackCrash (180397e50) sets the request
        // as soon as the chord is down. 1804c31ba consumes it and zeros the gauges.
        s.shortcutAt = inputAt;
        s.forcedCrash = true;
        s.chart.forceCrash();
        s.finish(now);
    } else if (!chord && s.shortcutAt >= 0) {
        if (inputAt - s.shortcutAt > PremiumRetryHoldMs) s.quickRetry();
        s.shortcutAt = -1;
    }
}

std::optional<Result> Stage::takeResult() {
    return std::exchange(impl_->result, std::nullopt);
}

const PlayOptions& Stage::options() const { return impl_->options; }

} // namespace sdvx
