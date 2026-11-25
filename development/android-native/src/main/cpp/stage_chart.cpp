#include "stage_chart.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <sstream>
#include <random>
#include <ranges>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace sdvx {
namespace {

// 18047c5e7 divides .125 by the range in the original normalized space.
// StageLaser positions already include the range transform.
constexpr float knobTolerance = .125f;
constexpr double contactGraceMs = 40, matchedDirectionMs = 80;
constexpr double cursorPreviewMs = 350; // Visual lead observed before the right phrase in the cabinet capture.
constexpr int tabFractionBits = 20, tabControlBits = tabFractionBits + 8;
constexpr uint32_t tabFractionMask = (1u << tabFractionBits) - 1;
constexpr uint32_t tabControlMask = (1u << tabControlBits) - 1;
constexpr double liveEqDelay = 80; // GameAudio 1804b7eb0, control queue age in ms.
// GameAudio 180644b30 indexes the frequency table at 18097b1c0.
constexpr std::array<int, 128> liveEqFrequencies{
    0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 100, 106, 112, 118, 124, 130,
    136, 142, 148, 154, 160, 166, 172, 178, 184, 190, 196, 202, 232, 262, 292, 322,
    352, 382, 412, 442, 472, 522, 572, 622, 672, 722, 772, 822, 872, 922, 972, 1022,
    1072, 1122, 1172, 1222, 1272, 1322, 1372, 1422, 1472, 1522, 1572, 1622, 1672, 1722, 1772, 1822,
    1872, 1922, 1972, 2022, 2072, 2122, 2172, 2222, 2272, 2322, 2372, 2422, 2472, 2522, 2572, 2622,
    2672, 2722, 2772, 2822, 2872, 2922, 2972, 3022, 3072, 3122, 3172, 3222, 3272, 3322, 3372, 3422,
    3472, 3522, 3572, 3622, 3672, 3852, 4032, 4212, 4392, 4572, 4752, 4932, 5112, 5292, 5472, 5652,
    5832, 6012, 6192, 6372, 6552, 6732, 6912, 7400, 7700, 8000, 8400, 8800, 9270, 9750, 10240, 10800,
};

int value(const nlohmann::json& fields, size_t index, int fallback = 0) {
    if (index >= fields.size()) return fallback;
    const auto& item = fields[index];
    return item.is_number_integer() ? item.get<int>() : std::stoi(item.get<std::string>());
}

float clampKnob(float position, float range) {
    return std::clamp(position, .5f - range * .5f, .5f + range * .5f);
}

int64_t frameAt(double ms) {
    return std::llround(ms * stageSampleRate / 1000);
}

} // namespace

float StageLaser::range() const { return std::max(start.range, end.range); }

float StageLaser::positionAt(double tick) const {
    if (start.tick == end.tick) return end.position;
    const auto t = std::clamp((tick - start.tick) / (end.tick - start.tick), 0.0, 1.0);
    return static_cast<float>(std::lerp(start.position, end.position, t));
}

StageChart::StageChart(const nlohmann::json& source, bool mirror, bool excessive, bool ars, bool autoPlay,
                       double permissiveRate, int randomMode, uint32_t seed, bool ultra)
    : endMs(source.at("chart_end").at("time_ms").get<double>()),
      ticksPerQuarter_(source.at("ticks_per_quarter").get<int>()), ars_(ars), autoPlay_(autoPlay),
      permissive_(permissiveRate >= 0) {
    stats_.ultra = ultra && !permissive_;
    stats_.excessive = (excessive || ultra) && !permissive_;
    stats_.gauge = permissive_ ? permissiveRate : stats_.excessive ? 1.0 : 0.0;
    knobs_[1].position = 1;

    if (const auto it = source.find("tempo_map"); it != source.end()) {
        for (const auto& row : *it) {
            const Tempo tempo{row.at("tick").get<int>(), row.at("time_ms").get<double>(), row.at("bpm").get<double>()};
            tempos_.push_back(tempo);
        }
    } else {
        tempos_.push_back({0, 0, source.at("bpm").get<double>()});
    }
    if (ticksPerQuarter_ < 4 || tempos_.empty()) throw std::runtime_error("Invalid VOX tempo map");
    // VOX 1802d77c0: the tick grid restarts at meter changes and selects
    // quarter/eighth-beat spacing using the BPM at each grid point.
    std::vector<int> meterStarts{0}, meterNumerators{4}, grid;
    if (source.contains("meter_map")) {
        meterStarts.clear();
        meterNumerators.clear();
        for (const auto& meter : source.at("meter_map")) {
            meterStarts.push_back(meter.at("tick"));
            meterNumerators.push_back(meter.value("numerator", 4));
        }
    }
    const int endTick = source.at("chart_end").at("tick");
    for (size_t m = 0; m < meterStarts.size(); ++m) {
        const int end = m + 1 < meterStarts.size() ? meterStarts[m + 1] : endTick;
        for (int tick = meterStarts[m]; tick < end;) {
            grid.push_back(tick);
            tick += ticksPerQuarter_ / (bpmAt(timeAt(tick)) >= 255 ? 2 : 4);
        }
    }
    const auto checkTicks = [&](int begin, int end, int step) {
        std::vector<double> times;
        if (step == 0) {
            for (auto tick = std::lower_bound(grid.begin(), grid.end(), begin); tick != grid.end() && *tick < end; ++tick)
                times.push_back(timeAt(*tick));
        } else {
            for (size_t m = 0; m < meterStarts.size() && meterStarts[m] < end; ++m) {
                const int stop = m + 1 < meterStarts.size() ? std::min(end, meterStarts[m + 1]) : end;
                const int first = meterStarts[m] + std::max(0, (begin - meterStarts[m] + step - 1) / step) * step;
                for (int tick = first; tick < stop; tick += step) times.push_back(timeAt(tick));
            }
        }
        return times;
    };

    const auto sections = source.find("other_vox_sections");
    const auto meters = source.find("meter_map");
    const bool sourcePostEffects = sections != source.end() && sections->contains("POSTEFFECT") && meters != source.end();
    for (const auto& row : source.at("controllers")) {
        const auto& name = row.at("name").get_ref<const std::string&>();
        // Prior exporters mirrored these rows and lost target metadata and millisecond durations.
        if (sourcePostEffects && (name == "ChromaticAbberationStrength" || name == "RadicalBlurStrength" ||
            name == "ChromaticAberrationStrength" || name == "CrtMonitorEffectActive" ||
            name == "CrtMonitorEffectDistortion" || name == "SetFrameLabel")) continue;
        cues.push_back({
            name,
            row.at("time_ms").get<double>(),
            timeAt(row.at("tick").get<int>() + row.at("duration_ticks").get<int>()),
            row.at("parameters").get<std::vector<std::string>>(), row.value("mode",0)
        });
    }
    if (sourcePostEffects) {
        const auto resolution = sections->find("BEAT RESOLUTION");
        const int tickScale = ticksPerQuarter_ /
            (resolution == sections->end() ? 48 : std::stoi(resolution->front().get<std::string>()));
        for (const auto& row : sections->at("POSTEFFECT")) {
            std::istringstream fields(row.get<std::string>());
            std::string position, duration, effect, parameter, first, last;
            int mode;
            unsigned target, index;
            if (!(fields >> position >> mode >> duration >> effect >> target >> index >> parameter >> first >> last))
                continue;
            int bar, beat, offset;
            char comma;
            std::istringstream positionFields(position);
            if (!(positionFields >> bar >> comma >> beat >> comma >> offset)) continue;
            const auto meter = std::ranges::find_if(*meters | std::views::reverse,
                [bar](const auto& entry) { return entry.at("bar").template get<int>() <= bar; });
            if (meter == meters->rend()) continue;
            const int beats = (bar - meter->at("bar").get<int>()) * meter->at("numerator").get<int>() + beat - 1;
            const int tick = meter->at("tick").get<int>() + beats * (4 * ticksPerQuarter_ / meter->at("denominator").get<int>())
                           + offset * tickScale;
            const auto start = timeAt(tick);
            if (effect == "SetFrameLabel") {
                cues.push_back({effect, start, start, {std::to_string(target), std::move(parameter)}, mode});
                continue;
            }
            if (target >= 2 || index >= 2) continue;
            const auto end = duration.ends_with("ms") ? start + std::stod(duration)
                : timeAt(tick + std::stoi(duration) * tickScale);
            cues.push_back({effect + parameter, start, end, {std::move(first), std::move(last)}, mode,
                            std::nullopt, StageCue::PostTarget{target, index}});
        }
        std::stable_sort(cues.begin(), cues.end(), [](const auto& left, const auto& right) { return left.time < right.time; });
    }
    // BuildTilt 180459520 uses the third parameter to group keys. Only the
    // standalone row or the closing row supplies its end value and duration.
    std::vector<TiltKey> tiltGroup;
    for (const auto& cue : cues) {
        if (cue.name != "Tilt") continue;
        const int mode = static_cast<int>(std::stof(cue.parameters.at(2)));
        const bool starts = tiltGroup.empty();
        if (starts && (mode < 0 || mode > 2)) continue;
        const auto target = [&](size_t index) {
            const float value = std::stof(cue.parameters.at(index));
            return std::abs(value) <= .01f ? 0.f : value * (mirror ? 2.375f : -2.375f);
        };
        tiltGroup.push_back({cue.time, target(0)});
        if ((starts && mode == 1) || mode == 3) {
            tiltGroup.push_back({cue.end, target(1)});
            tiltCurves_.push_back(std::move(tiltGroup));
            tiltGroup.clear();
        }
    }
    if (tiltGroup.size() >= 2) tiltCurves_.push_back(std::move(tiltGroup));
    if (source.contains("meter_map")) {
        const auto& meters = source.at("meter_map");
        size_t control = 0;
        bool hidden = false;
        for (size_t m = 0; m < meters.size(); ++m) {
            const auto& meter = meters[m];
            const int end = m + 1 < meters.size() ? meters[m + 1].at("tick").get<int>() : endTick;
            const int width = ticksPerQuarter_ * 4 * meter.value("numerator",4) / meter.value("denominator",4);
            for (int tick = meter.at("tick"); tick < end; tick += width) {
                const auto time = timeAt(tick);
                while (control < cues.size() && cues[control].time <= time) {
                    const auto& cue = cues[control++];
                    if (cue.name == "BAROFF") hidden = cue.parameters[0] == "ON";
                }
                if (!hidden) barLines.push_back(time);
            }
        }
    }
    int voxVersion = 10;
    double gaugeMultiplier = 1;
    if (const auto sections = source.find("other_vox_sections"); sections != source.end()) {
        if (const auto version = sections->find("FORMAT VERSION"); version != sections->end())
            voxVersion = value(*version, 0, voxVersion);
        if (const auto option = sections->find("BPM OPTION"); option != sections->end()) {
            bool constant = false;
            double bpm = tempos_.front().bpm;
            for (const auto& row : *option) {
                std::istringstream fields(row.get_ref<const std::string&>());
                std::string name;
                fields >> name;
                if (name == "ConstantScroll") fields >> constant;
                else if (name == "RepresentativeBpm") fields >> bpm;
            }
            if (constant) representativeBpm_ = bpm;
        }
        if (const auto option = sections->find("GAUGE OPTION"); option != sections->end()) {
            for (const auto& row : *option) {
                std::istringstream fields(row.get_ref<const std::string&>());
                std::string name;
                fields >> name;
                if (name == "TotalGaugeMag") fields >> gaugeMultiplier;
            }
        }
        const auto load = [&](const char* name, size_t rowsPerSlot, std::vector<Preset>& presets) {
            const auto section = sections->find(name);
            if (section == sections->end()) return;
            for (size_t index = 0; index < section->size(); index += rowsPerSlot) {
                Preset preset{};
                const auto& line = (*section)[index].get_ref<const std::string&>();
                const char* field = line.c_str();
                for (float& parameter : preset) {
                    char* end;
                    parameter = std::strtof(field, &end);
                    if (*end != ',') break;
                    field = end + 1;
                }
                presets.push_back(preset);
            }
        };
        // FXBUTTON stores two rows per slot. The mobile DSP currently consumes the first.
        load("FXBUTTON EFFECT INFO", 2, buttonPresets_);
        load("TAB EFFECT INFO", 1, tabPresets_);
        std::vector<Preset> assignments;
        load("TAB PARAM ASSIGN INFO", 1, assignments);
        tabAssignments_.resize(tabPresets_.size());
        // 1806c2c50 looks up the two parameter rows under the laser filter ID minus two.
        for (size_t i = 0; i < assignments.size(); ++i) {
            const auto& row = assignments[i];
            const int preset = static_cast<int>(row[0]) + 1;
            if (row[1] > 0 && preset >= 0 && static_cast<size_t>(preset) < tabAssignments_.size())
                tabAssignments_[preset][i % 2] = {static_cast<int>(row[1]), row[2], row[3]};
        }
    }
    // 1804574a4 merges ManualSpeed with the tempo map for the draw clock.
    // Judgment and DSP continue to use the VOX tempo map directly.
    std::vector<std::pair<double, double>> manual;
    for (const auto& cue : cues) {
        if (cue.name != "ManualSpeed") continue;
        const auto& parameter = cue.parameters.at(0);
        manual.emplace_back(cue.time, std::stod(parameter.starts_with('f') ? parameter.substr(1) : parameter));
    }
    for (size_t mode = 0; mode < scroll_.size(); ++mode) {
        auto& sections = scroll_[mode];
        const double fixedBpm = mode ? 100. : representativeBpm_;
        const auto rate = [&](double time) { return (fixedBpm > 0 ? fixedBpm : bpmAt(time)) * .008; };
        std::vector<double> changes{0};
        if (fixedBpm <= 0)
            for (const auto& tempo : tempos_) changes.push_back(static_cast<float>(tempo.time));
        for (const auto& entry : manual) changes.push_back(entry.first);
        std::ranges::sort(changes);
        double multiplier = 1;
        size_t next = 0;
        for (const double time : changes) {
            if (!sections.empty() && sections.back().time == time) continue;
            double position = time * rate(time);
            if (!sections.empty()) {
                const auto& previous = sections.back();
                position = previous.position + (time - previous.time) * previous.unitsPerMs;
            }
            while (next < manual.size() && manual[next].first <= time) multiplier = manual[next++].second;
            sections.push_back({time, position, rate(time) * multiplier});
        }
    }
    for (auto& preset : tabPresets_) {
        if (preset[0] != 1 && preset[0] != 2 && preset[0] != 3) preset[0] = 0;
    }
    tabHistory_.resize(tabPresets_.size());

    static constexpr const char* tableNames[] = {
        "chip", "fx_chip_special", "hold_wide", "hold_narrow", "laser_track", "laser_slam"
    };
    const auto& tables = source.at("judgment").at("tables");
    for (size_t i = 0; i < windows_.size(); ++i) {
        for (const auto& row : tables.at(tableNames[i])) {
            windows_[i].push_back({row.at("from_seconds").get<float>(), row.at("result_id").get<int>()});
        }
        if (windows_[i].empty()) throw std::runtime_error("Empty VOX judgment window");
    }

    const auto addCheck = [&](Check type, size_t object, double time, Window window) {
        checks_.push_back({type, object, time, time + windows_[window].back().seconds * 1000.0, type != Tail});
    };
    std::vector<std::array<int, 3>> holdTicks;
    const auto& events = source.at("events");
    const auto buttonTrack = [](int track) { return track == 2 ? 4 : track == 7 ? 5 : track - 3; };
    std::array<int, 6> buttonMap = mirror ? std::array{3, 2, 1, 0, 5, 4} : std::array{0, 1, 2, 3, 4, 5};
    std::minstd_rand random(seed);
    if (randomMode == 1 || randomMode == 3) {
        std::shuffle(buttonMap.begin(), buttonMap.begin() + 4, random);
        std::shuffle(buttonMap.begin() + 4, buttonMap.end(), random);
    }
    const bool superRandom = randomMode == 4;
    std::vector<int> buttonLanes;
    if (superRandom) {
        // Keep connected long notes together. Objects which overlap in time
        // retain separate lanes, including all members of a same-tick chord.
        struct Object { int source, start, end; std::vector<size_t> events; };
        std::vector<Object> objects;
        std::array<int, 6> lastObject;
        lastObject.fill(-1);
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& row = events[i];
            const int track = row.at("track"), tick = row.at("tick");
            if (track < 2 || track > 7) continue;
            const auto tail = row.find("hold_end");
            const int end = tail == row.end() ? tick : tail->at("tick").get<int>();
            const int source = buttonTrack(track);
            auto& last = lastObject[source];
            if (last < 0 || objects[last].end < tick) {
                last = static_cast<int>(objects.size());
                objects.push_back({source, tick, end, {i}});
            } else {
                objects[last].end = std::max(objects[last].end, end);
                objects[last].events.push_back(i);
            }
        }
        buttonLanes.resize(events.size());
        std::array<int, 6> busyUntil;
        busyUntil.fill(std::numeric_limits<int>::min());
        for (const auto& object : objects) {
            const int begin = object.source < 4 ? 0 : 4;
            const int end = object.source < 4 ? 4 : 6;
            std::array<int, 4> free;
            int count = 0;
            for (int lane = begin; lane < end; ++lane)
                if (busyUntil[lane] < object.start) free[count++] = lane;
            const int lane = free[std::uniform_int_distribution<int>(0, count - 1)(random)];
            busyUntil[lane] = object.end;
            for (const auto index : object.events) buttonLanes[index] = lane;
        }
    }
    std::vector<float> laserScales(events.size(), 1);
    std::vector<bool> laserMirrors(events.size(), mirror);
    struct LaserPhrase { int start, end; std::vector<size_t> events; };
    std::vector<LaserPhrase> laserPhrases;
    // FumenDataFromVoxFile::BuildTumami (180458290) selects AIRL/AIRR
    // by the phrase's range-group ID, not by the physical knob. The scale
    // is sampled at the phrase head and remains fixed through that phrase.
    for (int track : {1, 8}) {
        std::vector<size_t> phrase;
        int group = 1;
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& row = events[i];
            if (row.at("track").get<int>() != track) continue;
            const auto& fields = row.at("fields");
            phrase.push_back(i);
            group = std::max(group, value(fields, 4, 1));
            if (value(fields, 1) != 2) continue;
            float scale = 1;
            const double start = events[phrase.front()].at("time_ms");
            const auto name = group == 1 ? "AIRL_ScaX" : group == 2 ? "AIRR_ScaX" : "";
            for (const auto& cue : cues) {
                if (cue.time > start) break;
                if (cue.name == name) scale = std::stof(cue.parameters.at(1));
            }
            for (const auto index : phrase) laserScales[index] = scale;
            if (superRandom) laserPhrases.push_back({events[phrase.front()].at("tick"), row.at("tick"), phrase});
            phrase.clear();
            group = 1;
        }
    }
    if (superRandom) {
        // A continuous laser is one object. Overlapping phrases must move
        // together so two curves never occupy the same knob simultaneously.
        std::ranges::sort(laserPhrases, {}, &LaserPhrase::start);
        int until = std::numeric_limits<int>::min();
        bool swap = false;
        for (const auto& phrase : laserPhrases) {
            if (phrase.start > until) swap = std::uniform_int_distribution<int>(0, 1)(random) != 0;
            until = std::max(until, phrase.end);
            for (const auto index : phrase.events) laserMirrors[index] = swap;
        }
    }
    std::array<std::optional<LaserPoint>, 2> previous;
    std::array<std::optional<double>, 2> laserEnd;
    std::array<double, 2> sectionStart{};
    for (size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex) {
        const auto& row = events[eventIndex];
        const int track = row.at("track").get<int>();
        const int tick = row.at("tick").get<int>();
        const double time = row.at("time_ms").get<double>();
        const auto& fields = row.at("fields");
        if (track == 1 || track == 8) {
            const bool swapped = laserMirrors[eventIndex];
            const int side = (track == 1 ? 0 : 1) ^ int(swapped);
            const float range = laserScales[eventIndex];
            const auto& coordinate = fields.at(0);
            float normalized = coordinate.is_number() ? coordinate.get<float>() : std::stof(coordinate.get_ref<const std::string&>());
            // VOX 1802d2b90 uses 7-bit coordinates before v12 and floats from v12 onward.
            if (voxVersion < 12) normalized = normalized == 63 || normalized == 64 ? .5f : normalized / 127.f;
            const float position = (normalized - .5f) * range + .5f;
            const int type = value(fields, 1);
            // A section boundary inside the notice window belongs to the same phrase.
            const bool startsPhrase = type == 1 && !previous[side];
            if (startsPhrase) sectionStart[side] = time;
            const bool phraseHead = startsPhrase && (!laserEnd[side] || time - *laserEnd[side] >= laserNoticeLeadMs);
            // VOX 1802d50d0/1802d5186 parse the explicit tick interval into offset 0x3c.
            LaserPoint point{time, swapped ? 1 - position : position, tick, range, value(fields, 3), phraseHead,
                value(fields, voxVersion >= 13 ? 9 : 8), startsPhrase, sectionStart[side]};
            if (phraseHead) laserStarts.emplace_back(side, time);
            if (type != 1 && previous[side]) {
                const auto start = *previous[side];
                // BuildTumami 180458290 folds small legacy coordinate differences.
                // A coincident tail marker must keep the preceding endpoint.
                if (voxVersion < 12 && start.tick == tick && std::abs(point.position - start.position) <= .020685039f * range)
                    point.position = start.position;
                const size_t id = lasers.size();
                const StageLaser laser{side, start, point, time};
                lasers.push_back(laser);
                knobs_[side].lasers.push_back(id);
                if (laser.slam()) {
                    addCheck(Slam, id, time, LaserSlam);
                }
            }
            if (type == 2) laserEnd[side] = time;
            previous[side] = type == 2 ? std::nullopt : std::optional(point);
            continue;
        }
        if (track < 2 || track > 7) throw std::runtime_error("Invalid VOX button track");
        const int button = superRandom ? buttonLanes[eventIndex] : buttonMap[buttonTrack(track)];
        const auto tail = row.find("hold_end");
        const bool hold = tail != row.end();
        const double end = hold ? tail->at("time_ms").get<double>() : time;
        const int effect = button >= 4 ? value(fields, 1) : 0;
        const size_t id = notes.size();
        StageNote note{button, time, end, effect, hold ? 0 : effect, 1, 0, hold};
        if (hold && effect >= 2 && static_cast<size_t>(effect - 2) < buttonPresets_.size()) {
            const auto& p = buttonPresets_[effect - 2];
            const int type = static_cast<int>(p[0]);
            const double beatFrames = stageSampleRate * 60 / bpmAt(time);
            double beats = 1;
            switch (type) {
                case 1: case 8: {
                    const int divisions = std::clamp(static_cast<int>(p[1]), 1, 32);
                    const double periodBeats = p[3];
                    const int period = static_cast<int>(std::clamp(periodBeats * beatFrames, .1 * stageSampleRate, 8. * stageSampleRate));
                    note.cycle = period / divisions;
                    // 1806bd2f0 aligns Retrigger to the active tempo/meter grid.
                    // RetriggerEx instead takes a lookback in beats (1806c03b0).
                    if (type == 1) {
                        const auto meter = std::prev(std::upper_bound(meterStarts.begin(), meterStarts.end(), tick));
                        const auto& tempo = tempoAt(time);
                        const auto origin = std::max(tempo.time, timeAt(*meter));
                        const int beatsPerMeasure = meterNumerators[meter - meterStarts.begin()];
                        const auto inMeasure = (frameAt(time) - frameAt(origin))
                            % std::max<int64_t>(1, static_cast<int64_t>(beatFrames * beatsPerMeasure));
                        const int lead = static_cast<int>(inMeasure % std::max<int64_t>(1, std::llround(periodBeats * beatFrames)));
                        if (lead <= period - 512. * stageSampleRate / 44100) note.fxLeadFrames = lead;
                    } else {
                        note.fxLeadFrames = static_cast<int>(beatFrames * p[7]);
                    }
                    break;
                }
                case 2: beats = p[3]; break;
                case 3: {
                    // 1806c0d10 derives the LFO rate from the meter, BPM and VOX multiplier.
                    const auto meter = std::prev(std::upper_bound(meterStarts.begin(), meterStarts.end(), tick));
                    const int beatsPerMeasure = meterNumerators[meter - meterStarts.begin()];
                    beats = 2 * beatsPerMeasure / std::max(.01f, p[3]);
                    break;
                }
                case 5: beats = p[2]; break;
                case 6: beats = 1 / p[6]; break;
            }
            if (type != 1 && type != 8) {
                note.cycle = std::max(1, static_cast<int>(beats * beatFrames));
            }
            note.mix = std::clamp(p[type == 1 || type == 8 ? 2 : type == 6 ? 3 : 1], 0.f, 100.f) * .01f;
        } else if (hold) {
            note.preset = 0;
        }
        notes.push_back(note);
        holdTicks.push_back({tick, hold ? tail->at("tick").get<int>() : tick, value(fields, 2)});
        if (hold) {
            buttons_[button].holds.push_back(id);
        } else {
            buttons_[button].chips.push_back(id);
            ++totalChips_;
        }
    }
    // 1802d74a2/1802d75f3: connected FX segments share a hold head.
    // Remove up to two trailing grid points, leaving at least four interior
    // points. The tail changes input/FX state but is not a scoring note.
    for (const auto& button : buttons_) {
        std::vector<std::pair<size_t, double>> interior;
        double groupStart = 0;
        for (size_t i = 0; i < button.holds.size(); ++i) {
            const auto id = button.holds[i];
            auto& note = notes[id];
            const auto& ticks = holdTicks[id];
            const bool continues = i && holdTicks[button.holds[i - 1]][1] == ticks[0];
            if (!continues) {
                groupStart = note.start;
                addCheck(Head, id, note.start, HoldWide);
            } else note.headJudged = true;
            note.holdGroupStart = groupStart;
            for (double time : checkTicks(ticks[0] + !continues, ticks[1], ticks[2])) interior.emplace_back(id, time);
            addCheck(Tail, id, note.end, HoldNarrow);
            const bool follows = i + 1 < button.holds.size() && holdTicks[button.holds[i + 1]][0] == ticks[1];
            if (!follows) {
                const size_t remove = std::min<size_t>(2, interior.size() > 4 ? interior.size() - 4 : 0);
                interior.resize(interior.size() - remove);
                for (const auto& [object, time] : interior) addCheck(Hold, object, time, HoldNarrow);
                interior.clear();
            }
        }
    }
    // 1802db4c0/1802db530 remove regular ticks within 50 ms of the
    // adjacent slam, while keeping the slam's own checkpoint.
    for (const auto& knob : knobs_) {
        double phraseDeadline = 0;
        double directionEventAt = 0;
        double previousSlope = 0;
        int previousDirection = 0;
        for (size_t i = 0; i < knob.lasers.size(); ++i) {
            const auto id = knob.lasers[i];
            auto& laser = lasers[id];
            if (laser.start.startsPhrase) phraseDeadline = laser.start.time;
            const int direction = laser.direction();
            const double duration = laser.end.time - laser.start.time;
            const double slope = duration > 0 ? (laser.end.position - laser.start.position) * 1000.0 / (duration * laser.range()) : 0;
            // 18047b3a8/18047b550: a long run gets a direction event before
            // its turn. The event retains the outgoing direction and forces contact to stay on the curve.
            if (!laser.start.startsPhrase && !laser.slam() && previousDirection && direction != previousDirection &&
                laser.start.time - directionEventAt > 100) {
                const double assistAt = laser.start.time - (direction ? contactGraceMs : 20);
                for (size_t j = i; j > 0; --j) {
                    auto& outgoing = lasers[knob.lasers[j - 1]];
                    if (outgoing.end.time < assistAt) break;
                    outgoing.turnAssistAt = assistAt;
                }
            }
            if (laser.start.startsPhrase || laser.slam() || direction != previousDirection || std::abs(slope - previousSlope) > .7)
                directionEventAt = laser.start.time;
            previousDirection = laser.slam() ? 0 : direction;
            previousSlope = slope;
            if (laser.slam()) {
                phraseDeadline = std::max(phraseDeadline, laser.start.time + windows_[LaserSlam].back().seconds * 1000.0);
            } else {
                const auto* before = i ? &lasers[knob.lasers[i - 1]] : nullptr;
                const auto* after = i + 1 < knob.lasers.size() ? &lasers[knob.lasers[i + 1]] : nullptr;
                for (double time : checkTicks(laser.start.tick, laser.end.tick, laser.start.tickStep)) {
                    if (before && before->slam() && time < before->end.time + 50) continue;
                    if (after && after->slam() && time > after->start.time - 50) continue;
                    addCheck(Track, id, time, LaserTrack);
                    phraseDeadline = std::max(phraseDeadline, checks_.back().deadline);
                }
            }
            // 1804256c0: retirement covers the authored tail and the last judgment window.
            laser.cursorEnd = std::max(laser.end.time, phraseDeadline);
        }
    }
    std::stable_sort(checks_.begin(), checks_.end(), [autoPlay](const Checkpoint& a, const Checkpoint& b) {
        return autoPlay ? a.time < b.time : a.deadline < b.deadline;
    });

    int sustained = 0, slams = 0;
    for (size_t i = 0; i < checks_.size(); ++i) {
        const auto& check = checks_[i];
        sustained += check.scores;
        if (check.type == Slam) {
            knobs_[lasers[check.object].side].slams.push_back(i);
            slams += check.scores;
        }
        if (check.type == Track) knobs_[lasers[check.object].side].tracks.push_back(i);
    }
    stats_.total = totalChips_ + sustained;
    stats_.maxEx = 5 * totalChips_ + 2 * sustained;
    // 1804a7ad0 / 1804a9120: PERMISSIVE distributes 110 gauge points
    // across short buttons and slams (weight 1) and sustained checks (0.25).
    gaugeGain_ = permissive_ ? 440.f / std::max(1, (totalChips_ + slams) * 4 + sustained - slams)
                             : 2.1 / std::max(1, totalChips_ * 4 + sustained);
    ultraGain_ = 900.f / std::max(1, (totalChips_ + slams) * 4 + sustained - slams);
    // 180446714 applies the authored total multiplier on positive gauge changes.
    gaugeGain_ *= gaugeMultiplier;
    ultraGain_ *= gaugeMultiplier;
}

double StageChart::timeAt(double tick) const {
    const auto next = std::upper_bound(tempos_.begin(), tempos_.end(), tick,
        [](double target, const Tempo& tempo) { return target < tempo.tick; });
    const auto& tempo = next == tempos_.begin() ? tempos_.front() : *std::prev(next);
    return static_cast<float>(tempo.time + static_cast<double>(tick - tempo.tick) / ticksPerQuarter_ * (60000 / tempo.bpm));
}

const StageChart::Tempo& StageChart::tempoAt(double time) const {
    const auto next = std::upper_bound(tempos_.begin(), tempos_.end(), time,
        [](double target, const Tempo& tempo) { return target < static_cast<float>(tempo.time); });
    return next == tempos_.begin() ? tempos_.front() : *std::prev(next);
}

double StageChart::tickAt(double time) const {
    const auto& tempo = tempoAt(time);
    return tempo.tick + (time - tempo.time) * tempo.bpm * ticksPerQuarter_ / 60000;
}

double StageChart::bpmAt(double time) const { return tempoAt(time).bpm; }

double StageChart::scrollBpm(bool constantSpeed) const {
    return constantSpeed ? 100 : representativeBpm_ > 0 ? representativeBpm_ : bpmAt(0);
}

// DrawTimer::SetFumen (18014e390) integrates 8 * BPM units per second.
double StageChart::scrollPosition(double time, bool constantSpeed) const {
    const auto& sections = scroll_[constantSpeed];
    const auto next = std::ranges::upper_bound(sections, time, {}, &ScrollSection::time);
    const auto& section = next == sections.begin() ? sections.front() : *std::prev(next);
    return section.position + (time - section.time) * section.unitsPerMs;
}

double StageChart::scrollTime(double position, bool constantSpeed) const {
    const auto& sections = scroll_[constantSpeed];
    const auto next = std::ranges::upper_bound(sections, position, {}, &ScrollSection::position);
    const auto& section = next == sections.begin() ? sections.front() : *std::prev(next);
    if (section.unitsPerMs > 0) return section.time + (position - section.position) / section.unitsPerMs;
    return position < section.position ? section.time : std::numeric_limits<double>::infinity();
}

int StageChart::resultAt(Window table, double input, double object) const {
    const float delta = static_cast<float>(input / 1000) - static_cast<float>(object / 1000);
    int result = 0;
    for (const auto& boundary : windows_[table]) {
        if (delta < boundary.seconds) break;
        result = boundary.result;
    }
    return result;
}

StageChart::Window StageChart::chipWindow(const StageNote& note) const {
    return note.button >= 4 && note.sample != 0 ? FxChip : Chip;
}

StageNote* StageChart::currentHold(Button& button) {
    return button.nextHold < button.holds.size() ? &notes[button.holds[button.nextHold]] : nullptr;
}

bool StageChart::holding(int index) const {
    const auto& button = buttons_[index];
    return button.nextHold < button.holds.size() && notes[button.holds[button.nextHold]].active;
}

const StageLaser* StageChart::currentLaser(const Knob& knob) const {
    return knob.nextLaser < knob.lasers.size() ? &lasers[knob.lasers[knob.nextLaser]] : nullptr;
}

StageChart::TiltTarget StageChart::tiltTarget(double time) const {
    const auto group = std::ranges::upper_bound(tiltCurves_, time, {}, [](const auto& keys) {
        return keys.front().time;
    });
    if (group != tiltCurves_.begin()) {
        const auto& keys = *std::prev(group);
        const auto next = std::ranges::upper_bound(keys, time, {}, &TiltKey::time);
        if (next != keys.end()) {
            const auto& key = *std::prev(next);
            const float progress = (time - key.time) / (next->time - key.time);
            return {std::lerp(key.value, next->value, progress), 2.f};
        }
    }

    // AngleUpdater 18044b3bf consumes the chart's unscaled cursor path.
    // Each side contributes displacement from its own outer edge.
    float target = 0;
    for (int side = 0; side < 2; ++side) {
        const auto& segments = knobs_[side].lasers;
        const auto next = std::ranges::upper_bound(segments, time, {}, [&](size_t index) {
            return lasers[index].start.time;
        });
        if (next == segments.begin()) continue;
        const auto& laser = lasers[*std::prev(next)];
        if (time >= laser.end.time) continue;
        const float progress = (time - laser.start.time) / (laser.end.time - laser.start.time);
        const float position = std::lerp(laser.start.position, laser.end.position, progress);
        target += (position - .5f) / laser.range() + .5f - side;
    }
    return {target, 1.f};
}

std::optional<float> StageChart::cursorForDraw(int side, double time) const {
    const auto& knob = knobs_[side];
    const auto* laser = currentLaser(knob);
    if (knob.entered) {
        // 18047addc draws connected cursors on the visual chart clock.
        // Keep the cursor in the segment reached by the judgment clock.
        if (knob.active && !laser->slam()) return laser->positionAt(tickAt(time));
        return knob.position;
    }
    if (laser && laser->start.time - time <= cursorPreviewMs) return laser->start.position;
    return std::nullopt;
}

void StageChart::setFeedback(int button, int result) {
    feedback_[button] = result;
    ++feedbackSequence_[button];
}

void StageChart::judgeChip(StageNote& note, int result) {
    note.judged = true;
    record(result, ChipObject);
    setFeedback(note.button, result);
}

void StageChart::publishHold(int button, const Button& state, const StageNote* hold) {
    if (button < 4) return;
    const int index = (state.down || autoPlay_) && hold && hold->active && hold->preset != 0
        ? static_cast<int>(hold - notes.data()) : -1;
    holds_[button - 4].store(index, std::memory_order_relaxed);
}

StageChart::KnobAudio StageChart::tabControl(int side, const Knob& knob, const StageLaser* laser, double time) const {
    if (!laser || !knob.active || laser->start.time > time) return {};
    const int id = laser->start.filter;
    const auto inward = [&](float x) {
        const float position = (x - .5f) / laser->range() + .5f;
        return std::clamp(side ? 1 - position : position, 0.f, 1.f);
    };
    if (id == 0) {
        // TumamiNote 180477b40 evaluates the chart curve once the knob is in contact.
        return {0, static_cast<int>(127 * inward(laser->positionAt(tickAt(time)))), false};
    }
    if (id < 0 || id > 255 || static_cast<size_t>(id) > tabPresets_.size() || tabPresets_[id - 1][0] == 0)
        return {0, 0, true};
    const auto& assignments = tabAssignments_[id - 1];
    const bool assigned = assignments[0].parameter || assignments[1].parameter;
    const float position = assigned ? laser->positionAt(tickAt(time)) : knob.position;
    const auto fraction = static_cast<uint32_t>(inward(position) * tabFractionMask);
    return {(static_cast<uint32_t>(id) << tabFractionBits) | fraction, 0, true};
}

void StageChart::publishTabs(double time) {
    const auto& [left, right] = knobAudio_;
    if (left.authored || right.authored) {
        pendingEq_.clear();
        queuedEq_ = liveEq_ = 0;
    } else {
        const int position = std::max(left.position, right.position);
        if (position != queuedEq_) {
            queuedEq_ = position;
            pendingEq_.emplace_back(time, position);
        }
        while (!pendingEq_.empty() && time - pendingEq_.front().first > liveEqDelay) {
            liveEq_ = pendingEq_.front().second;
            pendingEq_.pop_front();
        }
    }
    const auto snapshot = uint64_t{left.preset} | (uint64_t{right.preset} << tabControlBits) |
                          (static_cast<uint64_t>(liveEq_) << (2 * tabControlBits));
    tabControls_.store(snapshot, std::memory_order_relaxed);
}

int StageChart::laserMaterial(const StageLaser& laser) const {
    const auto& knob = knobs_[laser.side];
    const auto* current = currentLaser(knob);
    if (!knob.entered || advancedTo_ < laser.start.sectionStart ||
        current->start.sectionStart != laser.start.sectionStart) return 0;
    // Sustained contact lights every segment of the current section.
    return knob.active ? 1 : 2;
}

ButtonPress StageChart::press(int index, double time) {
    advance(time);
    auto& button = buttons_[index];
    if (button.down) return {};
    button.down = true;
    button.pressedAt = time;
    auto* hold = currentHold(button);
    if (hold) {
        hold->active = time >= hold->start + windows_[HoldWide].front().seconds * 1000.0 && time <= hold->end;
        publishHold(index, button, hold);
        if (hold->active && !hold->headJudged && resultAt(HoldWide, time, hold->start) == judgmentCritical) {
            hold->headJudged = true;
            record(judgmentCritical, HoldObject);
        }
    }
    // Keep unresolved chip checkpoints in chart order. The original referee
    // consumes the lane's queued presses while evaluating each checkpoint.
    auto* chip = button.nextChip < button.chips.size() ? &notes[button.chips[button.nextChip]] : nullptr;
    const int result = chip ? resultAt(chipWindow(*chip), time, chip->start) : 0;
    if (hold && hold->active && (result == 0 || result == 1 || result == judgmentMiss)) {
        setFeedback(index, judgmentCritical);
        return {.judgment = judgmentCritical};
    }
    if (result == 0 || result == judgmentMiss) return {};
    judgeChip(*chip, result);
    if (result < 2 || result > 8) return {.judgment = result};
    return {result, chip->sample, chip->start - time};
}

void StageChart::release(int index, double time) {
    advance(time);
    releaseButton(index, time);
}

void StageChart::releaseButton(int index, double time) {
    auto& button = buttons_[index];
    if (!button.down) return;
    button.down = false;
    button.presses.emplace_back(button.pressedAt, time);
    if (auto* hold = currentHold(button)) hold->active = false;
    publishHold(index, button, nullptr);
}

void StageChart::clearInputs(double time) {
    advance(time);
    for (int index = 0; index < 6; ++index) releaseButton(index, time);
    for (auto& knob : knobs_) {
        knob.heldDirection = 0;
        knob.directionUntil = advancedTo_;
        knob.active = false;
    }
    knobAudio_ = {};
    pendingEq_.clear();
    queuedEq_ = liveEq_ = 0;
    tabControls_.store(0, std::memory_order_relaxed);
}

const StageLaser* StageChart::rotate(int side, float delta, double time) {
    if (delta == 0) return nullptr;
    advance(time);
    auto& knob = knobs_[side];
    if (!knob.entered) return nullptr;
    const auto* laser = currentLaser(knob);
    const int direction = delta > 0 ? 1 : -1;
    while (knob.nextSlam < knob.slams.size() && checks_[knob.slams[knob.nextSlam]].resolved) ++knob.nextSlam;
    Checkpoint* best = nullptr;
    double closest = std::numeric_limits<double>::infinity();
    for (size_t i = knob.nextSlam; i < knob.slams.size(); ++i) {
        auto& candidate = checks_[knob.slams[i]];
        if (time < candidate.time + windows_[LaserSlam].front().seconds * 1000.0) break;
        const double distance = std::abs(time - candidate.time);
        const auto& laser = lasers[candidate.object];
        if (!candidate.resolved && laser.direction() == direction && time < candidate.deadline && distance < closest &&
            std::abs(knob.position - laser.start.position) < knobTolerance) {
            best = &candidate;
            closest = distance;
        }
    }
    if (best) {
        auto& hit = lasers[best->object];
        best->resolved = true;
        record(judgmentCritical, LaserObject, true);
        if (advancedTo_ < hit.start.time) {
            hit.pendingSnap = true;
            return &hit;
        }
        knob.active = true;
        knob.position = hit.end.position;
        knob.heldDirection = 0;
        knob.directionUntil = advancedTo_;
        knobAudio_[side] = tabControl(side, knob, &hit, advancedTo_);
        publishTabs(advancedTo_);
        return &hit;
    }
    if (time < laser->start.time) return nullptr;
    const float target = laser->slam() ? laser->start.position : laser->positionAt(tickAt(advancedTo_));
    const float moved = clampKnob(knob.position + delta / RotaryUnitsPerLane, laser->range());
    if (laser->slam()) {
        knob.position = moved;
        knob.active = false;
    } else {
        const int chartDirection = laser->direction();
        const bool holdingDirection = advancedTo_ < knob.directionUntil;
        if (!holdingDirection || direction == chartDirection) knob.heldDirection = direction;

        const bool beforeTurn = laser->turnAssistAt && advancedTo_ >= *laser->turnAssistAt &&
                                advancedTo_ < laser->end.time;
        if (knob.active && (!chartDirection || beforeTurn)) {
            // 18047c867: keep contact and take the raw direction during the approach to the turn.
            knob.position = target;
            knob.heldDirection = direction;
            knob.directionUntil = time + contactGraceMs;
        } else if (holdingDirection && !knob.heldDirection) {
            knob.position = target;
        } else if (chartDirection && knob.heldDirection == chartDirection) {
            // 18047c66a: an attached cursor follows, and an approaching cursor stops at the line.
            const bool crosses = (target - knob.position) * direction > 0 && (moved - target) * direction >= 0;
            knob.position = knob.active || crosses ? target : moved;
            if (direction == chartDirection && std::abs(knob.position - target) < knobTolerance)
                knob.directionUntil = time + matchedDirectionMs;
        } else {
            knob.position = moved;
        }
        knob.active = std::abs(knob.position - target) < knobTolerance;
    }
    if (knob.active) markLaserContact(knob, advancedTo_, advancedTo_, false);
    knobAudio_[side] = tabControl(side, knob, laser, advancedTo_);
    publishTabs(advancedTo_);
    return nullptr;
}

void StageChart::markLaserContact(Knob& knob, double from, double to, bool following) {
    const auto object = knob.lasers[knob.nextLaser];
    const auto& laser = lasers[object];
    const double early = windows_[LaserTrack].front().seconds * 1000.0;
    while (knob.nextTrack < knob.tracks.size() && checks_[knob.tracks[knob.nextTrack]].deadline < from)
        ++knob.nextTrack;
    for (size_t i = knob.nextTrack; i < knob.tracks.size(); ++i) {
        auto& check = checks_[knob.tracks[i]];
        const double start = std::max(from, check.time + early);
        const double end = std::min(to, check.deadline);
        if (check.time + early > to) break;
        if (check.contacted || check.object != object || start > end) continue;
        const float a = laser.positionAt(tickAt(start)), b = laser.positionAt(tickAt(end));
        check.contacted = following || (knob.position > std::min(a,b) - knobTolerance &&
                                         knob.position < std::max(a,b) + knobTolerance);
    }
}

StageChart::KnobAudio StageChart::advanceKnob(int side, double from, double to) {
    auto& knob = knobs_[side];
    const auto* laser = currentLaser(knob);
    while (laser) {
        // The cabinet's cursor waits for the next phrase packet (18047bf40).
        // Arm the new phrase within the chart's early slam window.
        const double arm = laser->start.time + (laser->start.startsPhrase ? windows_[LaserSlam].front().seconds * 1000.0 : 0.);
        if (to < arm) break;
        if (!knob.entered) {
            knob.entered = true;
            if (laser->start.startsPhrase) {
                knob.position = laser->start.position;
                knob.active = false;
                knob.direction = 0;
                knob.heldDirection = 0;
                knob.directionUntil = laser->slam() ? laser->start.time : laser->start.time + contactGraceMs;
            }
        }
        if (laser->pendingSnap && to >= laser->start.time) {
            knob.position = laser->end.position;
            knob.active = true;
            knob.heldDirection = 0;
            knob.directionUntil = laser->start.time;
            lasers[knob.lasers[knob.nextLaser]].pendingSnap = false;
        }
        const double start = std::max(from, laser->start.time);
        const double end = std::min(to, laser->end.time);
        if (autoPlay_) {
            if (to >= laser->start.time) knob.position = laser->positionAt(tickAt(end));
        } else if (start <= end && !laser->slam()) {
            const int direction = laser->direction();
            const auto track = [&](double first, double last) {
                const float target = laser->positionAt(tickAt(first));
                const bool touching = std::abs(knob.position - target) < knobTolerance;
                double followEnd = first;
                if (touching && !direction) {
                    followEnd = last;
                    knob.position = target;
                    knob.heldDirection = 0;
                    knob.directionUntil = last + contactGraceMs;
                } else if (touching && (!knob.heldDirection || knob.heldDirection == direction ||
                                        (laser->turnAssistAt && first >= *laser->turnAssistAt))) {
                    followEnd = std::clamp(knob.directionUntil, first, last);
                    if (followEnd > first) knob.position = laser->positionAt(tickAt(followEnd));
                }
                if (followEnd > first) markLaserContact(knob, first, followEnd, true);
                // Check the swept interval after follow expires, including contacts between frames.
                markLaserContact(knob, followEnd, last, false);
            };
            double first = start;
            if (laser->turnAssistAt && start <= *laser->turnAssistAt && from < *laser->turnAssistAt &&
                *laser->turnAssistAt <= end) {
                const double assistAt = *laser->turnAssistAt;
                track(first, assistAt);
                const float target = laser->positionAt(tickAt(assistAt));
                if (knob.heldDirection && assistAt < knob.directionUntil &&
                    std::abs(knob.position - target) < knobTolerance) {
                    knob.position = target;
                    knob.heldDirection = 0;
                    knob.directionUntil = assistAt + contactGraceMs;
                }
                first = assistAt;
            }
            track(first, end);
        }
        if (start <= end) {
            const float target = laser->positionAt(tickAt(end));
            if (!laser->slam() || autoPlay_) knob.active = std::abs(knob.position - target) < knobTolerance;
            if (laser->direction() != knob.direction && !laser->slam() && knob.active)
                knob.corner = {laser->start.time, target, laser->direction()};
            knob.direction = laser->direction();
        }
        const bool phraseTail = knob.nextLaser + 1 == knob.lasers.size() ||
            lasers[knob.lasers[knob.nextLaser + 1]].start.startsPhrase;
        double retire = phraseTail ? laser->cursorEnd : laser->end.time;
        if (autoPlay_ && phraseTail && knob.nextLaser + 1 < knob.lasers.size())
            retire = std::min(retire, lasers[knob.lasers[knob.nextLaser + 1]].start.time);
        // A turn at the exact corner belongs to the incoming direction. Slam and
        // phrase boundaries still accept their existing late judgments.
        const bool keepBoundary = !autoPlay_ && (phraseTail || laser->slam() ||
            lasers[knob.lasers[knob.nextLaser + 1]].slam());
        if (to < retire || (to == retire && keepBoundary)) break;
        // 18047c0a1: the exit animation belongs to the retiring phrase.
        if (phraseTail && knob.active) knob.exit = {retire, knob.position};
        from = std::max(from, laser->end.time);
        knob.entered = false;
        ++knob.nextLaser;
        laser = currentLaser(knob);
    }
    if (!knob.entered) {
        knob.active = false;
        knob.position = static_cast<float>(side);
        knob.heldDirection = 0;
        knob.directionUntil = to;
    }
    return tabControl(side, knob, laser, to);
}

void StageChart::advance(double time) {
    if (time < advancedTo_) return;
    if (autoPlay_) {
        autoFeedback_.samples.clear();
        autoFeedback_.slams.clear();
    }
    for (int side = 0; side < 2; ++side) knobAudio_[side] = advanceKnob(side, advancedTo_, time);
    publishTabs(time);
    advancedTo_ = time;
    for (auto& button : buttons_) {
        while (button.nextChip < button.chips.size()) {
            auto& note = notes[button.chips[button.nextChip]];
            if (!note.judged) {
                if (autoPlay_) {
                    if (time < note.start) break;
                    judgeChip(note, 6);
                    if (note.sample != 0) autoFeedback_.samples.push_back(&note);
                } else {
                    if (resultAt(chipWindow(note), time, note.start) != judgmentMiss) break;
                    judgeChip(note, judgmentMiss);
                }
            }
            ++button.nextChip;
        }
    }
    while (nextCheck_ < checks_.size() && (autoPlay_ ? checks_[nextCheck_].time : checks_[nextCheck_].deadline) <= time) {
        auto& check = checks_[nextCheck_++];
        const int result = judge(check);
        if (check.scores && result != 0)
            record(result, check.type >= Track ? LaserObject : HoldObject, check.type == Slam);
    }
    for (int i = 0; i < 6; ++i) {
        auto& button = buttons_[i];
        while (button.nextHold < button.holds.size()) {
            auto& note = notes[button.holds[button.nextHold]];
            if (autoPlay_ ? note.end > time : note.end >= time) break;
            note.active = false;
            ++button.nextHold;
        }
        auto* hold = currentHold(button);
        if (hold) {
            const double earliest = hold->holdGroupStart + windows_[HoldWide].front().seconds * 1000.0;
            hold->active = autoPlay_ ? time >= hold->start && time < hold->end
                : button.down && button.pressedAt >= earliest && time >= earliest && time <= hold->end;
        }
        publishHold(i, button, hold);
        while (!button.presses.empty() && button.presses.front().second < time - 500) button.presses.pop_front();
    }
}

int StageChart::judge(Checkpoint& check) {
    if (autoPlay_) {
        if (check.type == Head) {
            auto& note = notes[check.object];
            note.headJudged = true;
            setFeedback(note.button, judgmentCritical);
        } else if (check.type == Tail) {
            notes[check.object].judged = true;
        } else if (check.type == Slam) autoFeedback_.slams.push_back(&lasers[check.object]);
        return judgmentCritical;
    }
    if (check.type == Head) {
        auto& note = notes[check.object];
        if (note.headJudged) return 0;
        note.headJudged = true;
        return judgmentMiss;
    }
    if (check.type == Hold || check.type == Tail) {
        auto& note = notes[check.object];
        const auto& button = buttons_[note.button];
        const double start = std::max(note.start, check.time + windows_[HoldNarrow].front().seconds * 1000.0);
        const double end = std::min(note.end, check.deadline);
        const double earliest = note.holdGroupStart + windows_[HoldWide].front().seconds * 1000.0;
        double held = 0;
        for (const auto& [down, up] : button.presses) {
            if (down >= earliest) held += std::max(0.0, std::min(end, up) - std::max(start, down));
        }
        if (button.down && button.pressedAt >= earliest) held += std::max(0.0, end - std::max(start, button.pressedAt));
        if (check.type == Tail) {
            note.active = false;
            note.judged = true;
        }
        return held > (end - start) * .75 ? judgmentCritical : judgmentMiss;
    }
    if (check.resolved) return 0;
    check.resolved = true;
    return check.type == Track && check.contacted ? judgmentCritical : judgmentMiss;
}

void StageChart::record(int result, Kind kind, bool slam) {
    ++results_[kind][result];
    const bool chip = kind == ChipObject;
    const bool hit = result >= 2 && result <= 8;
    const bool near = result == 2 || result == 8;
    if (hit) {
        if (near) ++stats_.near;
        else ++stats_.critical;
        stats_.maxChain = std::max(stats_.maxChain, ++stats_.chain);
    } else {
        ++stats_.errors;
        stats_.chain = 0;
    }
    if (failedRate_) return;
    if (permissive_) {
        // 180446690 and its note-weight callbacks apply the table in percentage points.
        const float rate = static_cast<float>(stats_.gauge * 100.);
        const float weight = chip || slam ? 1.f : .25f;
        const float lossScale = rate < 30.f ? .3f + (.7f * rate) / 30.f : 1.f;
        const float change = hit ? static_cast<float>(gaugeGain_) * (near ? 1.f / 3.f : 1.f)
                                 : -3.4f * lossScale;
        const float next = std::clamp(rate + change * weight, 0.f, 100.f);
        stats_.gauge = next / 100.;
        failedRate_ = next == 0.f;
        return;
    }

    const double effectiveChange = hit ? gaugeGain_ * (chip ? 4 : 1) * (near ? 1. / 3 : 1)
                                       : chip ? -.02 : -.005;
    effectiveGauge_ = std::clamp(effectiveGauge_ + effectiveChange, 0.0, 1.0);
    if (stats_.excessive && excessiveGauge_ > 0) {
        excessiveGauge_ = std::clamp(excessiveGauge_ + effectiveChange * (hit ? .55 : 2), 0.0, 1.0);
    }
    if (stats_.ultra) {
        // 1804a9120 mode 6 uses the original judgement ID, including the two kinds of ERROR.
        constexpr std::array<float, 11> changes{0, -20, -4, 0, 1, 1, 1, 0, -4, -20, -20};
        const float change = changes[result];
        const float weight = chip || slam ? 1.f : .25f;
        const float rate = static_cast<float>(stats_.gauge * 100.);
        // 180446690 uses the 0.01-point floor for judgements 2–8, including NEAR.
        const float next = std::clamp(rate + change * weight * (change > 0 ? static_cast<float>(ultraGain_) : 1.f),
                                      hit ? .01f : 0.f, 100.f);
        stats_.gauge = next / 100.;
        if (next > 0) return;
        if (!ars_) {
            failedRate_ = true;
            return;
        }
        stats_.ultra = false;
    }
    if (stats_.excessive && excessiveGauge_ == 0) {
        if (ars_) stats_.excessive = false;
        else failedRate_ = true;
    }
    stats_.gauge = stats_.excessive ? excessiveGauge_ : effectiveGauge_;
}

int StageChart::score() const {
    return stats_.total == 0 ? 0 : static_cast<int>((2LL * stats_.critical + stats_.near) * 5000000 / stats_.total);
}

int StageChart::exScore() const {
    const auto& chips = results_[ChipObject];
    return 5 * (chips[4] + chips[6]) + 4 * (chips[3] + chips[5] + chips[7]) +
           2 * (chips[2] + chips[8] + results_[HoldObject][judgmentCritical] + results_[LaserObject][judgmentCritical]);
}

ClearType StageChart::clearType(bool showS, bool forcedCrash) const {
    if (forcedCrash || failedRate_) return ClearType::Crash;
    if (stats_.errors == 0 && stats_.critical + stats_.near == stats_.total) {
        if (stats_.near != 0) return ClearType::Ultimate;
        const auto& chips = results_[ChipObject];
        return showS && chips[4] + chips[6] == totalChips_ ? ClearType::SPerfect : ClearType::Perfect;
    }
    return permissive_ || stats_.excessive || stats_.gauge >= .7 ? ClearType::Complete : ClearType::Crash;
}

void StageChart::processAudio(float* samples, int frames, int64_t firstFrame, std::span<const int16_t> dryPcm) {
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float apply(float sample, FilterHistory& history) const {
            const float output = b0 * sample + b1 * history.x1 + b2 * history.x2
                - a1 * history.y1 - a2 * history.y2;
            history = {sample, history.x1, output, history.y1};
            return output;
        }
    };
    const auto coefficients = [](double hz, double q, bool highPass) {
        const double omega = 2 * std::numbers::pi * hz / stageSampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2 * q);
        const double reciprocal = 1 / (1 + alpha);
        const auto b0 = static_cast<float>((1 + (highPass ? cosine : -cosine)) * .5 * reciprocal);
        return Biquad{b0, (highPass ? -2.f : 2.f) * b0, b0,
                      static_cast<float>(-2 * cosine * reciprocal), static_cast<float>((1 - alpha) * reciprocal)};
    };
    struct HoldEffect {
        const Preset* parameters = nullptr;
        int type = 0, cycle = 1, subdivisions = 1, onFrames = 0, fadeFrames = 0, stopFrames = 0;
        int lead = 0, note = -1, flangerPasses = 0;
        int64_t start = 0, end = 0;
        float mix = 0, flangerDepth = 0, flangerDelay = 0, flangerSweep = 0, firstPassMix = 0;
        std::array<float, 32> repeatGains{};
        Biquad filter{};
    };
    struct TabEffect {
        TabHistory* history = nullptr;
        int type = 0, reduction = 1;
        float mix = 0, gain = 1;
        Biquad filter{};
    };
    std::array<HoldEffect, 2> holds;
    std::array<TabEffect, 2> tabs;
    const auto snapshot = tabControls_.load(std::memory_order_relaxed);
    std::array<uint32_t, 2> controls{static_cast<uint32_t>(snapshot) & tabControlMask,
                                  static_cast<uint32_t>(snapshot >> tabControlBits) & tabControlMask};
    // The original combines simultaneous knob control by maximum inward travel,
    // not by processing the same filter twice (which doubles its attenuation).
    if ((controls[0] >> tabFractionBits) != 0 &&
        (controls[0] >> tabFractionBits) == (controls[1] >> tabFractionBits)) {
        controls[0] = std::max(controls[0], controls[1]);
        controls[1] = 0;
    }
    const std::array ids{controls[0] >> tabFractionBits, controls[1] >> tabFractionBits};
    // A preset keeps its stereo history until both knobs have released it.
    for (const auto id : activeTabIds_) {
        if (id != 0 && id != ids[0] && id != ids[1]) tabHistory_[id - 1] = {};
    }
    activeTabIds_ = ids;
    const auto eqPosition = static_cast<int>(snapshot >> (2 * tabControlBits));
    const bool live = eqPosition >= 4;
    Biquad liveFilter{};
    float liveVolume = 1;
    if (live) {
        // 180644b30 configures DirectSound's parametric EQ in Hz, semitones, and dB.
        const double hz = std::clamp(liveEqFrequencies[eqPosition], 80, 16000);
        const double band = hz < 200 ? hz * .075 : 15 - std::max(0., hz - 1000) * .0003;
        const double boost = hz < 200 ? hz * .075 : 15 - std::max(0., hz - 1000) * .0005;
        const double omega = 2 * std::numbers::pi * hz / stageSampleRate;
        const double sine = std::sin(omega);
        const double alpha = sine * std::sinh(std::numbers::ln2 * band * omega / (24 * sine));
        const double a = std::pow(10., boost / 40);
        const double scale = 1 / (1 + alpha / a);
        const auto middle = static_cast<float>(-2 * std::cos(omega) * scale);
        liveFilter = {static_cast<float>((1 + alpha * a) * scale), middle,
                      static_cast<float>((1 - alpha * a) * scale), middle,
                      static_cast<float>((1 - alpha / a) * scale)};
        liveVolume = eqPosition < 95 ? .8f - (eqPosition - 4) * .0025274728f
            : eqPosition < 100 ? .57f : eqPosition < 120 ? .57f + (eqPosition - 100) * .0115f : .8f;
    } else {
        liveEqHistory_ = {};
    }
    bool active = live;
    for (int side = 0; side < 2; ++side) {
        const int index = holds_[side].load(std::memory_order_relaxed);
        int wobble = -1;
        if (index >= 0) {
            const auto& note = notes[index];
            const auto& p = buttonPresets_[note.preset - 2];
            auto& effect = holds[side];
            effect.parameters = &p;
            effect.type = static_cast<int>(p[0]);
            effect.cycle = note.cycle;
            effect.start = frameAt(note.start);
            effect.end = frameAt(note.end);
            effect.mix = note.mix;
            effect.lead = note.fxLeadFrames;
            effect.note = index;
            if (effect.type == 1 || effect.type == 8) {
                effect.subdivisions = std::clamp(static_cast<int>(p[1]), 1, 32);
                effect.onFrames = static_cast<int>(effect.cycle * std::clamp(p[5], .1f, 1.f));
                effect.fadeFrames = static_cast<int>(effect.onFrames * std::clamp(p[6], 0.f, 1.f));
                const float decay = std::clamp(p[4], .1f, 1.f);
                effect.repeatGains[0] = 1;
                for (int repeat = 1; repeat < effect.subdivisions; ++repeat)
                    effect.repeatGains[repeat] = effect.repeatGains[repeat - 1] * decay;
            } else if (effect.type == 2) {
                effect.cycle = std::max(1, effect.cycle / std::clamp(static_cast<int>(p[2]), 1, 32));
            } else if (effect.type == 3) {
                effect.flangerDepth = std::clamp(p[5], 0.f, 4.f);
                effect.flangerPasses = 1 + static_cast<int>(std::ceil(effect.flangerDepth));
                effect.flangerDelay = std::clamp(p[2], .1f, 3.f) * (stageSampleRate * .001f);
                effect.flangerSweep = std::clamp(p[4], 0.f, 100.f) * .01f;
                effect.firstPassMix = effect.mix - (1 - effect.mix) * (effect.flangerPasses - 1 - effect.flangerDepth);
            } else if (effect.type == 4) {
                effect.stopFrames = static_cast<int>(std::clamp(p[3], .1f, 2.f) * stageSampleRate);
            } else if (effect.type == 6) {
                wobble = index;
            }
            active = true;
        }
        if (wobble != activeWobbleNotes_[side]) {
            wobbleHistory_[side] = {};
            activeWobbleNotes_[side] = wobble;
        }
        if (holds[side].type != 3) flangerHistory_[side].note = -1;
        const auto tab = controls[side];
        const auto id = ids[side];
        if (id != 0) {
            const auto& p = tabPresets_[id - 1];
            const auto position = static_cast<float>(tab & tabFractionMask) / tabFractionMask;
            const int type = static_cast<int>(p[0]);
            auto& effect = tabs[side];
            effect.history = &tabHistory_[id - 1];
            effect.type = type;
            const float phase = type == 1 ? 1 - position : position;
            float mix = p[1], q = p[4];
            // 1806bfa40 uses a 1–30 sweep and truncates the unassigned value.
            float reduction = 1 + 29 * position;
            double hz = type == 3 ? 0 : p[2] * std::pow(p[3] / p[2], phase);
            for (const auto& assignment : tabAssignments_[id - 1]) {
                const auto [parameter, from, to] = assignment;
                if (parameter == 1) mix = std::lerp(from, to, phase);
                else if (parameter == 2) {
                    if (type == 3) reduction = std::round(std::lerp(from, to, phase));
                    // A fixed 0 Hz assignment uses the cabinet filter's 1 Hz floor.
                    else if (from == to) hz = std::max(1.f, from);
                    else if (from > 0 && to > 0) hz = from * std::pow(to / from, phase);
                } else if (parameter == 3 && type != 3) q = std::lerp(from, to, phase);
            }
            effect.mix = std::clamp(mix, 0.f, 100.f) * .01f;
            if (type == 3) {
                effect.reduction = std::clamp(static_cast<int>(reduction), 1, 30);
            } else {
                q = std::max(.1f, q);
                effect.filter = coefficients(hz, q, type == 2);
                // Keep the cabinet's signed attenuation within unity gain on the mobile mix.
                effect.gain = std::clamp(static_cast<float>(1 - .04 * q), -1.f, 1.f);
            }
            active = true;
        }
    }
    if (!active) return;
    const auto pcm = [dryPcm](int64_t frame, int channel) {
        const auto index = frame * 2 + channel;
        return index >= 0 && static_cast<uint64_t>(index) < dryPcm.size() ? dryPcm[index] / 32768.f : 0.f;
    };
    for (int i = 0; i < frames; ++i) {
        const int64_t frame = firstFrame + i;
        for (int channel = 0; channel < 2; ++channel) {
            float sample = samples[i * 2 + channel];
            for (int side = 0; side < 2; ++side) {
                auto& hold = holds[side];
                if (!hold.parameters || frame < hold.start || frame >= hold.end) continue;
                const auto& p = *hold.parameters;
                const int64_t time = frame - hold.start;
                const int64_t elapsed = time + hold.lead;
                const auto phase = static_cast<int>(elapsed % hold.cycle);
                const float fraction = static_cast<float>(phase) / hold.cycle;
                float wet = sample;
                switch (hold.type) {
                    case 1: case 8: {
                        // 1806d1c10 repeats the first slice, decays each repeat, and
                        // fades the end of the audible portion before gating it.
                        const int repeat = static_cast<int>((elapsed / hold.cycle) % hold.subdivisions);
                        const float envelope = phase > hold.onFrames ? 0.f : hold.fadeFrames == 0 ? 1.f
                            : std::min(1.f, static_cast<float>(hold.onFrames - phase) / hold.fadeFrames);
                        wet = envelope * hold.repeatGains[repeat] * pcm(frame - int64_t{repeat} * hold.cycle, channel);
                        break;
                    }
                    case 2: {
                        // The authored Gate uses the alternating 32/4 gain table
                        // at 180988a10. 1806d3980 scales each entry by .0322.
                        const int step = static_cast<int>((time / hold.cycle) % 2);
                        wet = sample * (step == 0 ? 1.0304f : .1288f);
                        break;
                    }
                    case 3: {
                        // 1806d1080 uses interpolated delay taps a quarter LFO
                        // period apart in stereo and cascades the authored passes.
                        auto& history = flangerHistory_[side];
                        if (channel == 0 && (history.note != hold.note || history.next != frame)) {
                            history.first = frame;
                            history.note = hold.note;
                        }
                        const double angle = fraction * 2 * std::numbers::pi + channel * std::numbers::pi / 2;
                        const double delay = hold.flangerDelay * (1 + hold.flangerSweep * std::sin(angle));
                        const int delayFrames = static_cast<int>(delay);
                        const float delayFraction = static_cast<float>(delay - delayFrames);
                        wet = sample;
                        for (int pass = 0; pass < hold.flangerPasses; ++pass) {
                            auto& input = history.input[pass];
                            input[frame % FlangerHistory::frames][channel] = wet;
                            const int64_t at = frame - delayFrames;
                            const float recent = at >= history.first ? input[at % FlangerHistory::frames][channel] : pcm(at, channel);
                            const float older = at - 1 >= history.first
                                ? input[(at - 1) % FlangerHistory::frames][channel] : pcm(at - 1, channel);
                            const float mix = pass == 0 ? hold.firstPassMix : hold.mix;
                            wet = std::lerp(wet, std::lerp(recent, older, delayFraction), mix);
                        }
                        if (hold.flangerDepth >= 1) wet *= 1.5f;
                        if (channel == 1) history.next = frame + 1;
                        break;
                    }
                    case 12: {
                        const double lfo = .5 - .5 * std::cos(fraction * 2 * std::numbers::pi);
                        const auto delay = 1 + static_cast<int>(lfo * 100);
                        const float delayed = pcm(std::max<int64_t>(0, frame - delay), channel);
                        wet = (sample + delayed) * .5f;
                        break;
                    }
                    case 6: {
                        // NABLA 1806c6270 -> 1806d3150: the bundled Wobble presets
                        // use a triangular sweep in log frequency. Refresh both
                        // channels from the same chart-relative sample position.
                        constexpr int coefficientFrames = 32;
                        if (channel == 0 && (i == 0 || time % coefficientFrames == 0)) {
                            const double sweep = static_cast<double>((time / coefficientFrames * coefficientFrames) % hold.cycle) / hold.cycle;
                            const double low = std::min(p[4], p[5]), high = std::max(p[4], p[5]);
                            const double hz = low * std::pow(high / low, 1 - std::abs(2 * sweep - 1));
                            hold.filter = coefficients(hz, p[7], false);
                        }
                        wet = hold.filter.apply(sample, wobbleHistory_[side][channel]);
                        break;
                    }
                    case 4: {
                        // 1806d2360 advances the wet source at a decreasing rate
                        // and fades to zero over the authored stop time.
                        wet = 0;
                        if (time < hold.stopFrames) {
                            const double slope = std::clamp(p[2], 1.f, 10.f) / hold.stopFrames;
                            const double b = 1 - slope * .5;
                            const double elapsed = std::max(0., time - 1. + slope);
                            // Invert the accumulated wait between source frames.
                            const auto source = std::min(time,
                                1 + static_cast<int64_t>(2 * elapsed / (b + std::sqrt(b * b + 2 * slope * elapsed))));
                            wet = pcm(hold.start + source, channel) * (1 - static_cast<float>(time) / hold.stopFrames);
                        }
                        break;
                    }
                    case 10: {
                        const auto progress = static_cast<float>(time) / std::max<int64_t>(1, hold.end - hold.start);
                        wet = pcm(hold.start + std::llround(time * (1 - progress * .5f)), channel) * (1 - progress);
                        break;
                    }
                    case 5: wet = sample * std::min(1.f, static_cast<float>(phase) / std::max<int64_t>(1, frameAt(p[4] + p[5]))); break;
                    case 7: wet = pcm(frame - phase % std::max(1, static_cast<int>(p[2])), channel); break;
                }
                const bool usesAuthoredEnvelope = hold.type == 1 || hold.type == 2 || hold.type == 3 ||
                                                  hold.type == 4 || hold.type == 8;
                const float edge = usesAuthoredEnvelope ? 1.f
                    : std::min(1.f, static_cast<float>(std::min(time, hold.end - frame)) / 80);
                sample += (wet - sample) * (hold.type == 3 ? 1.f : hold.mix) * edge;
                if (hold.type == 6) sample *= 1 - .04f * p[7] * edge;
            }
            for (int side = 0; side < 2; ++side) {
                const auto& tab = tabs[side];
                if (tab.type == 0) continue;
                float wet;
                if (tab.type == 3) {
                    // 1806d18c0 holds its input within each 512-frame cabinet DSP block.
                    // Keep the phase through BASS callbacks and consume preceding FX output.
                    constexpr int blockFrames = (512 * stageSampleRate + 22050) / 44100;
                    auto& history = *tab.history;
                    if (history.blockFrame == 0) history.reduction = tab.reduction;
                    if (history.blockFrame % history.reduction == 0) history.held[channel] = sample;
                    wet = history.held[channel];
                    if (channel == 1 && ++history.blockFrame == blockFrames) history.blockFrame = 0;
                } else {
                    wet = tab.filter.apply(sample, tab.history->filter[channel]);
                }
                sample = (sample + (wet - sample) * tab.mix) * tab.gain;
            }
            if (live) sample = liveFilter.apply(sample * liveVolume, liveEqHistory_[channel]);
            samples[i * 2 + channel] = std::clamp(sample, -1.f, 1.f);
        }
    }
}

} // namespace sdvx
