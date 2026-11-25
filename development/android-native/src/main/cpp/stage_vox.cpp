#include "stage_vox.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sdvx {
namespace {

using nlohmann::json;
using Fields = std::vector<std::string_view>;
constexpr std::string_view whitespace = " \t\r\n\v\f";
constexpr int defaultResolution = 48;

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(whitespace);
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

Fields split(std::string_view text) {
    Fields fields;
    for (text = trim(text); !text.empty(); text = trim(text.substr(fields.back().size()))) {
        fields.push_back(text.substr(0, text.find_first_of(whitespace)));
    }
    return fields;
}

int integer(std::string_view value) {
    if (value.starts_with('+')) value.remove_prefix(1);
    if (value.empty()) throw std::runtime_error("Empty VOX integer");
    int number = 0;
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end)
        throw std::runtime_error("Invalid VOX integer: " + std::string(value));
    return number;
}

int checkedInt(int64_t value) {
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::runtime_error("VOX tick exceeds the chart range");
    return static_cast<int>(value);
}

struct Position {
    int bar;
    int beat;
    int offset;
};

Position position(std::string_view text) {
    const auto first = text.find(',');
    const auto second = text.find(',', first == text.npos ? text.size() : first + 1);
    if (second == text.npos) throw std::runtime_error("Invalid VOX position: " + std::string(text));
    return {integer(text.substr(0, first)), integer(text.substr(first + 1, second - first - 1)),
            integer(text.substr(second + 1))};
}

struct Meter {
    int bar;
    int tick;
    int numerator;
    int denominator;
};

struct Tempo {
    int tick;
    double time;
    double bpm;
};

// Match the grid and Fumen float timestamp in tools/compile-vox.py.
struct VoxClock {
    int scale;
    int ticksPerQuarter;
    std::vector<Meter> meters;
    std::vector<Tempo> tempos;
    json meterMap = json::array();
    json tempoMap = json::array();

    VoxClock(const Fields& beats, const Fields& bpms, int resolution) {
        if (resolution <= 0 || beats.empty() || bpms.empty())
            throw std::runtime_error("Invalid VOX meter or tempo map");
        const int baseGrid = checkedInt(4LL * resolution);
        int grid = baseGrid;
        for (const auto row : beats) {
            const auto fields = split(row);
            if (fields.size() != 3) throw std::runtime_error("Invalid VOX meter row");
            const int denominator = integer(fields[2]);
            if (denominator <= 0) throw std::runtime_error("Invalid VOX meter denominator");
            grid = checkedInt(std::lcm(int64_t{grid}, int64_t{denominator}));
        }
        scale = grid / baseGrid;
        ticksPerQuarter = grid / 4;
        for (const auto row : beats) {
            const auto fields = split(row);
            const auto at = position(fields[0]);
            const int numerator = integer(fields[1]);
            const int denominator = integer(fields[2]);
            if (at.beat != 1 || at.offset || numerator <= 0)
                throw std::runtime_error("Unsupported VOX meter change");
            const int start = meters.empty() ? 0 : tick(at);
            meters.push_back({at.bar, start, numerator, denominator});
            meterMap.push_back({{"bar", at.bar}, {"tick", start}, {"numerator", numerator},
                                {"denominator", denominator}});
        }
        double time = 0;
        for (const auto row : bpms) {
            const auto fields = split(row);
            const int start = tick(position(fields.at(0)));
            const auto value = std::string(fields.at(1));
            size_t consumed = 0;
            const double bpm = std::stod(value, &consumed);
            if (consumed != value.size() || !std::isfinite(bpm) || bpm <= 0)
                throw std::runtime_error("Invalid VOX BPM: " + value);
            if (!tempos.empty()) time += elapsed(start, tempos.back());
            tempos.push_back({start, time, bpm});
            const Fields flags(fields.begin() + 2, fields.end());
            tempoMap.push_back({{"tick", start}, {"time_ms", time}, {"bpm", bpm}, {"flags", flags}});
        }
    }

    int tick(Position at) const {
        const auto next = std::upper_bound(meters.begin(), meters.end(), at.bar,
            [](int bar, const Meter& meter) { return bar < meter.bar; });
        const auto& meter = next == meters.begin() ? meters.back() : *std::prev(next);
        const int64_t beats = (int64_t{at.bar} - meter.bar) * meter.numerator + at.beat - 1;
        return checkedInt(meter.tick + int64_t{checkedInt(beats)} * (4 * ticksPerQuarter / meter.denominator)
                          + int64_t{at.offset} * scale);
    }

    double elapsed(int tick, const Tempo& tempo) const {
        return double(int64_t{tick} - tempo.tick) / ticksPerQuarter * (60'000 / tempo.bpm);
    }

    float milliseconds(int tick) const {
        const auto next = std::upper_bound(tempos.begin(), tempos.end(), tick,
            [](int at, const Tempo& tempo) { return at < tempo.tick; });
        const auto& tempo = next == tempos.begin() ? tempos.back() : *std::prev(next);
        return static_cast<float>(tempo.time + elapsed(tick, tempo));
    }
};

void sortByTick(json& rows) {
    std::stable_sort(rows.begin(), rows.end(), [](const json& left, const json& right) {
        return left.at("tick").get<int>() < right.at("tick").get<int>();
    });
}

} // namespace

nlohmann::json parseVox(std::string_view bytes, const nlohmann::json& judgment, int maxExscore) {
    std::map<std::string_view, Fields, std::less<>> sections;
    Fields* section = nullptr;
    while (!bytes.empty()) {
        const auto lineEnd = bytes.find_first_of("\r\n");
        auto line = bytes.substr(0, lineEnd);
        bytes.remove_prefix(lineEnd == bytes.npos ? bytes.size() : lineEnd + 1);
        // VOX grammar is ASCII. Drop comments at the byte level since source
        // charts use both CP932 and UTF-8 in their author annotations.
        line = trim(line.substr(0, line.find("//")));
        if (line.empty()) continue;
        if (line == "#END") section = nullptr;
        else if (line.starts_with('#')) section = &sections[line.substr(1)];
        else if (section) section->push_back(line);
    }
    const auto resolution = sections.find("BEAT RESOLUTION");
    const VoxClock clock(sections.at("BEAT INFO"), sections.at("BPM INFO"),
        resolution == sections.end() ? defaultResolution : integer(resolution->second.at(0)));
    const int version = integer(sections.at("FORMAT VERSION").at(0));

    json events = json::array();
    for (int track = 1; track <= 8; ++track) {
        const auto trackName = "TRACK" + std::to_string(track);
        const auto source = sections.find(trackName);
        if (source == sections.end()) continue;
        const bool laser = track == 1 || track == 8;
        const size_t step = laser ? (version >= 13 ? 9 : 8) : 2;
        for (const auto row : source->second) {
            const auto fields = split(row);
            const auto at = fields.at(0);
            const int start = clock.tick(position(at));
            json values = Fields(fields.begin() + 1, fields.end());
            if (clock.scale != 1 && values.size() > step)
                values[step] = std::to_string(checkedInt(int64_t{integer(fields[step + 1])} * clock.scale));
            json event = {{"track", track}, {"position", at}, {"tick", start},
                          {"time_ms", clock.milliseconds(start)}, {"fields", std::move(values)}};
            if (!laser) {
                const int length = integer(fields.at(1));
                if (length > 0) {
                    const int end = checkedInt(start + int64_t{length} * clock.scale);
                    event["hold_end"] = {{"tick", end}, {"time_ms", clock.milliseconds(end)}};
                }
            }
            events.push_back(std::move(event));
        }
        sections.erase(source);
    }
    sortByTick(events);

    json controls = json::array();
    for (const auto name : {"SPCONTROLER", "LOCKED_SPCONTROLER"}) {
        const auto source = sections.find(name);
        if (source == sections.end()) continue;
        for (const auto row : source->second) {
            const auto fields = split(row);
            const int start = clock.tick(position(fields.at(0)));
            const int mode = integer(fields.at(2));
            const int duration = checkedInt(int64_t{integer(fields.at(3))} * clock.scale);
            const Fields parameters(fields.begin() + 4, fields.end());
            controls.push_back({{"position", fields[0]}, {"name", fields[1]}, {"tick", start},
                {"time_ms", clock.milliseconds(start)}, {"mode", mode}, {"duration_ticks", duration},
                {"parameters", parameters}});
        }
        sections.erase(source);
    }
    if (const auto source = sections.find("POSTEFFECT"); source != sections.end()) {
        for (const auto row : source->second) {
            const auto fields = split(row);
            const auto name = fields.at(3);
            const auto label = fields.at(6);
            if (!((label == "Strength" && (name == "ChromaticAbberation" || name == "RadicalBlur"))
                  || (name == "CrtMonitorEffect" && (label == "Active" || label == "Distortion")))) continue;
            if (fields.size() < 9) throw std::runtime_error("Invalid VOX post-effect row");
            const int start = clock.tick(position(fields[0]));
            const auto duration = fields[2];
            const auto parameter = [](std::string_view value) {
                if (value.ends_with('f')) value.remove_suffix(1);
                return value;
            };
            controls.push_back({{"name", std::string(name) + std::string(label)}, {"position", fields[0]},
                {"tick", start}, {"time_ms", clock.milliseconds(start)}, {"mode", integer(fields[1])},
                {"duration_ticks", duration.ends_with("ms") ? 0 : checkedInt(int64_t{integer(duration)} * clock.scale)},
                {"parameters", json::array({parameter(fields[fields.size() - 2]), parameter(fields.back())})}});
        }
    }
    sortByTick(controls);
    const auto endPosition = sections.at("END POSITION").at(0);
    const int end = clock.tick(position(endPosition));
    const auto& meter = clock.meters.front();
    return {{"ticks_per_quarter", clock.ticksPerQuarter}, {"bpm", clock.tempos.front().bpm},
        {"meter", json::array({meter.numerator, meter.denominator})}, {"tempo_map", clock.tempoMap},
        {"meter_map", clock.meterMap}, {"chart_end", {{"position", endPosition}, {"tick", end},
            {"time_ms", clock.milliseconds(end)}}}, {"events", std::move(events)},
        {"controllers", std::move(controls)}, {"other_vox_sections", sections},
        {"judgment", judgment.at("judgment")}, {"offsets", judgment.at("offsets")}, {"max_exscore", maxExscore}};
}

} // namespace sdvx
