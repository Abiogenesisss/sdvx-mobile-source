#include "stage_automation.h"

#include "app.h"
#include "s3v_audio.h"
#include "stage_chart.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace sdvx {
namespace {
using Json = nlohmann::json;
constexpr int ticksPerBar = 192;
constexpr int headerBytes = 44, frameBytes = 4;
constexpr int fillBank = 99001, sweepBank = 99002;

struct Phrase {
    int id, wave, start, length, pre, post;
    std::string name, previous, next;
    unsigned attributes;
};

struct Segment {
    const Phrase* phrase;
    int at, length;
};

// At a source change, keep laser phrases and holds inside the chosen range.
// Keeping whole authored laser phrases also keeps zero-duration slams intact.
struct ChartSource {
    Json data;
    int fxBase, fxCount, tabBase, tabCount;
};

void appendCue(Json& chart, int tick, std::string_view name, std::vector<std::string> parameters) {
    chart["controllers"].push_back({{"name", name}, {"tick", tick},
        {"duration_ticks", 0}, {"parameters", std::move(parameters)}});
}

ChartSource readChart(Json& chart, const std::string& path) {
    auto source = Json::parse(readAsset(path));
    auto& sections = chart["other_vox_sections"];
    auto& presets = source["other_vox_sections"];
    auto& fx = sections["FXBUTTON EFFECT INFO"];
    auto& tab = sections["TAB EFFECT INFO"];
    const int fxBase = int(fx.size() / 2), tabBase = int(tab.size());
    auto& sourceFx = presets["FXBUTTON EFFECT INFO"];
    auto& sourceTab = presets["TAB EFFECT INFO"];
    const int fxCount = int(sourceFx.size() / 2), tabCount = int(sourceTab.size());
    for (auto& row : sourceFx) fx.push_back(std::move(row));
    for (auto& row : sourceTab) tab.push_back(std::move(row));
    for (const auto& row : presets["TAB PARAM ASSIGN INFO"]) {
        const auto& text = row.get_ref<const std::string&>();
        const auto comma = text.find(',');
        sections["TAB PARAM ASSIGN INFO"].push_back(std::to_string(std::stoi(text) + tabBase) + text.substr(comma));
    }
    return {std::move(source), fxBase, fxCount, tabBase, tabCount};
}

void spliceChart(Json& chart, const ChartSource& source, int from, int until, int at, double msPerTick) {
    auto& events = chart["events"];
    const auto copy = [&](Json row) {
        const int tick = row["tick"].get<int>() + at - from;
        row["tick"] = tick;
        row["time_ms"] = tick * msPerTick;
        auto& fields = row["fields"];
        const int track = row["track"];
        if (track == 1 || track == 8) {
            const int preset = std::stoi(fields[3].get_ref<const std::string&>());
            if (preset > 0 && preset <= source.tabCount) fields[3] = std::to_string(preset + source.tabBase);
        } else if (row.contains("hold_end")) {
            auto& tail = row["hold_end"];
            const int end = std::min(until, tail["tick"].get<int>()) + at - from;
            tail = {{"tick", end}, {"time_ms", end * msPerTick}};
            fields[0] = std::to_string(end - tick);
            if (track == 2 || track == 7) {
                const int preset = std::stoi(fields[1].get_ref<const std::string&>());
                if (preset >= 2 && preset < 2 + source.fxCount) fields[1] = std::to_string(preset + source.fxBase);
            }
        }
        events.push_back(std::move(row));
    };
    std::array<std::vector<const Json*>, 2> laser;
    for (const auto& row : source.data["events"]) {
        const int tick = row["tick"], track = row["track"];
        if (track != 1 && track != 8) {
            if (from <= tick && tick < until) copy(row);
            continue;
        }
        auto& phrase = laser[track == 8];
        const auto& type = row["fields"][1].get_ref<const std::string&>();
        if (type == "1") phrase.clear();
        phrase.push_back(&row);
        if (type != "2") continue;
        if (phrase.front()->at("tick").get<int>() >= from && tick < until)
            for (const auto* point : phrase) copy(*point);
        phrase.clear();
    }
    auto& controllers = chart["controllers"];
    for (auto row : source.data["controllers"]) {
        const int tick = row["tick"];
        if (tick < from || tick >= until) continue;
        // EXBG and SetFrameLabel in a source chart address that song's stage.
        const auto& parameters = row["parameters"];
        if (row["name"] == "SetFrameLabel" || (row["name"] == "SpecialN" && !parameters.empty() &&
            (parameters[0] == "EXBG" || parameters[0] == "EXBGTEX" || parameters[0] == "LIVE2D"))) continue;
        row["tick"] = tick + at - from;
        row["duration_ticks"] = std::min(row["duration_ticks"].get<int>(), until - tick);
        controllers.push_back(std::move(row));
    }
}
}

void prepareAutomationParadise(const Selection& selection, Json& chart, std::string& wave,
                               std::vector<int16_t>& dryPcm, uint32_t seed) {
    const auto config = Json::parse(readAsset(selection.presentation("arrangement.json")));
    const auto ticks = [](double bars) { return int(std::lround(bars * ticksPerBar)); };
    std::vector<Phrase> phrases;
    const auto& rows = config.at("tracks");
    for (const auto& track : rows) for (const auto& row : track.at("phrases")) {
        if (row.at("part") == "nachords") continue;
        phrases.push_back({track.at("id"), row.at("wave"), ticks(row.at("start").get<double>() - 1),
            ticks(row.at("length")), ticks(row.at("pre").get<double>() / 4),
            ticks(row.at("post").get<double>() / 4), row.at("name"), row.at("previous"), row.at("next"), row.at("attributes")});
    }
    const auto bpm = config.at("bpm").get<double>();
    const auto msPerTick = 60000 / (48 * bpm);
    constexpr unsigned startFlag = 0x400, finishFlag = 0x800, landFlag = 0x80, takeoffFlag = 0x100;
    std::vector<const Phrase*> starts, finishes, entries, fills;
    for (const auto& phrase : phrases) {
        if (phrase.id == fillBank) fills.push_back(&phrase);
        if (phrase.id >= fillBank) continue;
        if (phrase.attributes & startFlag) starts.push_back(&phrase);
        if (phrase.attributes & finishFlag) finishes.push_back(&phrase);
        if ((phrase.attributes & landFlag) && !(phrase.attributes & (startFlag | finishFlag)))
            entries.push_back(&phrase);
    }
    std::mt19937 random(seed);
    const auto choose = [&random](const auto& pool) {
        return pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(random)];
    };
    // The original setup scores trial plans by distinct source songs first
    // and distance from its requested duration second (18053af40).
    constexpr int targetMs = 165000;
    const int target = int(std::lround(targetMs / msPerTick));
    std::vector<Segment> best;
    std::pair<int, int> bestScore{-1, 0};
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::vector<Segment> route;
        std::vector<int> songs;
        const auto* opening = choose(starts);
        const auto* ending = choose(finishes);
        int at = opening->start;
        const auto append = [&](const Phrase* phrase) {
            route.push_back({phrase, at, phrase->length});
            at += phrase->length;
        };
        auto* current = opening;
        songs.push_back(opening->id);
        while (at < target - ending->length) {
            append(current);
            const auto successor = std::ranges::find_if(phrases, [current](const Phrase& candidate) {
                return candidate.id == current->id && candidate.name == current->next &&
                    candidate.previous == current->name &&
                    !(candidate.attributes & finishFlag);
            });
            if (at >= target - ending->length) break;
            if (successor != phrases.end() &&
                (current->post < 0 || successor->pre < 0 || !(current->attributes & takeoffFlag))) {
                current = &*successor;
                continue;
            }
            const auto remaining = target - at - ending->length;
            std::vector<const Phrase*> next;
            for (auto* phrase : entries) {
                if (phrase->pre >= 0 && phrase->length <= remaining + ticksPerBar * 2 &&
                    std::ranges::find(songs, phrase->id) == songs.end() && phrase->id != ending->id)
                    next.push_back(phrase);
            }
            if (next.empty()) break;
            // The common bank contains authored one-bar transition charts.
            if (remaining >= ticksPerBar * 6 && (current->attributes & 0x20)) append(choose(fills));
            current = choose(next);
            songs.push_back(current->id);
        }
        append(ending);
        if (std::ranges::find(songs, ending->id) == songs.end()) songs.push_back(ending->id);
        const std::pair score{int(songs.size()), -std::abs(at - target)};
        if (score > bestScore) {
            bestScore = score;
            best = std::move(route);
        }
    }

    const int end = best.back().at + best.back().length;
    chart = {{"ticks_per_quarter", 48}, {"bpm", bpm}, {"meter", {4, 4}},
             {"meter_map", {{{"bar", 1}, {"tick", 0}, {"numerator", 4}, {"denominator", 4}}}},
             {"chart_end", {{"tick", end}, {"time_ms", end * msPerTick}}},
             {"events", Json::array()}, {"controllers", Json::array()},
             {"judgment", config.at("judgment")}, {"offsets", config.at("offsets")},
             {"other_vox_sections", {{"FORMAT VERSION", {"10"}}}}};
    const auto frameAt = [msPerTick](int tick) { return int(std::llround(tick * msPerTick * stageSampleRate / 1000)); };
    dryPcm.resize(size_t(frameAt(end)) * 2);
    const int difficulty = selection.label == "NOV" ? 0 : selection.label == "ADV" ? 1 : selection.label == "EXH" ? 2 : 3;
    const auto pathFor = [&selection](const Phrase& phrase) {
        return selection.presentation(std::to_string(phrase.id) + "/" + std::to_string(phrase.wave));
    };
    std::string decoded, previousPath;
    std::unordered_map<std::string, ChartSource> charts;
    const auto sweepPhrase = std::ranges::find(phrases, sweepBank, &Phrase::id);
    const auto sweep = decodeS3vWave(readAsset(pathFor(*sweepPhrase) + ".s3v"), stageSampleRate);
    const int sweepLead = sweepPhrase->length;
    if (sweep.size() < headerBytes + size_t(frameAt(sweepLead)) * frameBytes)
        throw std::runtime_error("Incomplete Automation Paradise sweep");
    int featuredSong = 0;
    bool left = true;
    for (size_t first = 0; first < best.size();) {
        const auto& segment = best[first];
        const auto& phrase = *segment.phrase;
        const auto path = pathFor(phrase);
        size_t next = first + 1;
        int length = segment.length;
        while (next < best.size() && best[next].phrase->id == phrase.id &&
            best[next].phrase->wave == phrase.wave && best[next].phrase->start == phrase.start + length) {
            length += best[next++].length;
        }
        const int sourceStart = first == 0 ? 0 : phrase.start;
        const int targetStart = first == 0 ? 0 : segment.at;
        auto source = charts.find(path);
        if (source == charts.end())
            source = charts.emplace(path, readChart(chart, path + "-" + std::to_string(difficulty) + ".json")).first;
        spliceChart(chart, source->second, sourceStart, phrase.start + length, targetStart, msPerTick);
        if (path != previousPath) {
            decoded = decodeS3vWave(readAsset(path + ".s3v"), stageSampleRate);
            if (decoded.empty()) throw std::runtime_error("Unable to decode Automation Paradise source: " + path);
            previousPath = path;
        }
        const int sourceFrame = frameAt(sourceStart), targetFrame = frameAt(targetStart);
        const int frames = frameAt(segment.at + length) - targetFrame;
        // The common fill S3V is a silent carrier. Its VOX supplies the fill.
        const int available = std::max(0, int((decoded.size() - headerBytes) / frameBytes) - sourceFrame);
        const int copyFrames = std::min(frames, available);
        std::memcpy(dryPcm.data() + targetFrame * 2, decoded.data() + headerBytes + std::min(sourceFrame,
            int((decoded.size() - headerBytes) / frameBytes)) * frameBytes, size_t(copyFrames) * frameBytes);
        // At hard splices, taper the sample discontinuity without shifting the grid.
        constexpr int fade = stageSampleRate / 200;
        for (int i = 0; i < std::min(fade, copyFrames); ++i) {
            for (int channel = 0; channel < 2; ++channel) {
                if (first) dryPcm[(targetFrame + i) * 2 + channel] =
                    int16_t(dryPcm[(targetFrame + i) * 2 + channel] * i / fade);
                dryPcm[(targetFrame + copyFrames - i - 1) * 2 + channel] =
                    int16_t(dryPcm[(targetFrame + copyFrames - i - 1) * 2 + channel] * i / fade);
            }
        }
        if (phrase.id != fillBank && featuredSong != phrase.id) {
            appendCue(chart, segment.at, "AutomaPhrase", {std::to_string(phrase.id),
                featuredSong == 0 ? "solo" : left ? "solo_l_double" : "solo_r_double"});
            if (featuredSong != 0) {
                // An authored double panel briefly shows the departing and incoming jackets.
                appendCue(chart, segment.at + ticksPerBar, "AutomaPanel", {left ? "l_double_solo" : "r_double_solo"});
                left = !left;
            }
            featuredSong = phrase.id;
        }
        first = next;
    }
    // The common fill chart carries no music. Lay the authored sweep over that
    // transition, reaching its first phrase boundary when the next song enters.
    const auto* sweepPcm = sweep.data() + headerBytes;
    for (const auto& segment : best) if (segment.phrase->id == fillBank) {
        const int endTick = segment.at + segment.length;
        const int start = frameAt(std::max(0, endTick - sweepLead));
        const int from = frameAt(std::max(0, sweepLead - endTick));
        const int frames = frameAt(endTick) - start;
        for (int i = 0; i < frames * 2; ++i) {
            int16_t sample;
            std::memcpy(&sample, sweepPcm + (from * 2 + i) * sizeof(sample), sizeof(sample));
            auto& output = dryPcm[start * 2 + i];
            output = int16_t(std::clamp(int(output) + int(sample), -32768, 32767));
        }
    }
    appendCue(chart, end - ticksPerBar, "AutomaPhrase", {"0", "solo_u_out"});
    for (auto& cue : chart["controllers"]) cue["time_ms"] = cue["tick"].get<int>() * msPerTick;
    const auto byTick = [](const Json& lhs, const Json& rhs) { return lhs["tick"] < rhs["tick"]; };
    std::stable_sort(chart["events"].begin(), chart["events"].end(), byTick);
    std::stable_sort(chart["controllers"].begin(), chart["controllers"].end(), byTick);

    wave.assign(decoded.data(), headerBytes);
    wave.append(reinterpret_cast<const char*>(dryPcm.data()), dryPcm.size() * sizeof(int16_t));
    for (int i = 0; i < 4; ++i) {
        wave[4 + i] = char((wave.size() - 8) >> (i * 8));
        wave[40 + i] = char((wave.size() - headerBytes) >> (i * 8));
    }
}

} // namespace sdvx
