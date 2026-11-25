#include "cabinet_crew_catalog.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace sdvx {
namespace {
using Json = nlohmann::json;

const auto& voiceBanks() {
    static const auto banks = [] {
        std::unordered_map<std::string, std::unordered_map<std::string, CrewVoice>> result;
        const auto bundled = Json::parse(readAsset("crew/voice/index.json"));
        const auto external = Json::parse(readExternalAsset("crew/voice/index.json"), nullptr, false);
        for (const auto* source : {&bundled, &external}) {
            if (!source->is_object()) continue;
            for (const auto& [bank, slots] : source->items()) {
                auto& voices = result[bank];
                const std::string_view previous = bank == "vo032_rasis_nabra" ? "crew/rasis/" :
                    bank == "vo033_grace_nabra" ? "crew/grace/" : "";
                for (const auto& [name, slot] : slots.items()) {
                    if (voices.contains(name)) continue;
                    auto path = "crew/voice/" + slot.at(0).get<std::string>();
                    if (!previous.empty()) {
                        auto wav = std::string(previous) + name + ".wav";
                        if (hasAsset(wav)) path = std::move(wav);
                    }
                    voices.emplace(name, CrewVoice{std::move(path), std::pow(10.f, slot.at(1).get<float>() / 20)});
                }
            }
        }
        return result;
    }();
    return banks;
}

CrewInfo readCrew(const Json& row) {
    return {row.at("id"), row.at("style"), row.at("order"), row.at("icon"), row.value("preview", ""),
            row.value("name_image", ""), row.value("name", ""), row.value("portrait", ""),
            row.value("room_afp", ""), row.value("matching_afp", ""), row.value("voice", ""),
            row.value("actors", std::vector<int>{}), row.value("bundled", true),
            row.value("afp_motions", decltype(CrewInfo::afpMotions){})};
}

struct Catalog {
    std::vector<CrewInfo> crews;
    std::vector<CrewFamily> families;
    std::vector<CrewActor> actors;

    void appendActors(const Json& source) {
        for (const auto& actor : source) {
            CrewActor item{actor.value("model", ""), {}, {}};
            if (!item.model.empty()) {
                for (size_t scene = 0; scene < item.placements.size(); ++scene) {
                    const auto& placement = actor.at("placements").at(scene);
                    item.placements[scene] = {placement.at(0), placement.at(1), placement.at(2), placement.at(3)};
                }
                item.motions = actor.value("motions", decltype(item.motions){});
            }
            actors.push_back(std::move(item));
        }
    }

    Catalog() {
        const auto source = Json::parse(readAsset("crew/catalog.json"));
        const auto external = Json::parse(readExternalAsset("crew/catalog.json"), nullptr, false);
        appendActors(source.at("actors"));
        const int actorOffset = int(actors.size());
        const bool hasExternalCatalog = external.is_object() && external.contains("actors") && external.contains("crews")
            && external["actors"].is_array() && external["crews"].is_array();
        if (hasExternalCatalog) appendActors(external["actors"]);
        for (const auto& row : source.at("crews")) {
            auto item = readCrew(row);
            if (!item.installed && hasExternalCatalog) {
                const auto& additions = external["crews"];
                const auto match = std::ranges::find_if(additions, [&](const auto& other) {
                    return other.at("id") == item.id && other.at("style") == item.style;
                });
                if (match != additions.end()) {
                    auto imported = readCrew(*match);
                    const bool modelsReady = !imported.actors.empty() && std::ranges::all_of(imported.actors, [&](int index) {
                        return index >= 0 && index < int(actors.size()) - actorOffset
                            && hasExternalAsset(actors[actorOffset + index].model);
                    });
                    const bool animationReady = imported.actors.empty() && !imported.roomAfp.empty()
                        && (hasExternalAsset(imported.roomAfp + "/afp-library.json")
                            || hasExternalAsset(imported.roomAfp + "/timeline.dat"));
                    const bool portraitReady = imported.actors.empty() && !imported.portrait.empty()
                        && hasExternalAsset(imported.portrait);
                    const bool voiceReady = imported.voice.empty() || voiceBanks().contains(imported.voice);
                    if ((modelsReady || animationReady || portraitReady) && voiceReady) {
                        for (auto& index : imported.actors) index += actorOffset;
                        imported.installed = true;
                        item = std::move(imported);
                    }
                }
            }
            crews.push_back(std::move(item));
            const auto& crew = crews.back();
            auto family = std::find_if(families.begin(), families.end(), [&](const auto& item) { return item.id == crew.id; });
            if (family == families.end()) family = families.insert(families.end(), {crew.id, {}});
            family->styles.push_back(int(crews.size()) - 1);
        }
        std::sort(families.begin(), families.end(), [&](const auto& left, const auto& right) {
            const auto a = crews[left.styles.front()].order;
            const auto b = crews[right.styles.front()].order;
            return a == b ? left.id > right.id : a > b;
        });
    }
};

const Catalog& catalog() {
    static const Catalog data;
    return data;
}
}

const CrewInfo& crewInfo(int index) { return catalog().crews[index]; }
const CrewActor& crewActor(int index) { return catalog().actors[index]; }
const std::vector<CrewFamily>& crewFamilies() { return catalog().families; }

const CrewVoice* crewVoice(int crew, std::string_view cue) {
    const auto& banks = voiceBanks();
    const auto bank = banks.find(crewInfo(crew).voice);
    if (bank == banks.end()) return nullptr;
    const auto slot = bank->second.find(std::string(cue.substr(0, cue.rfind('.'))));
    return slot == bank->second.end() ? nullptr : &slot->second;
}

} // namespace sdvx
