#include "stage_scene.h"

#include "afp_movie.h"
#include "cabinet_crew_model.h"
#include "stage_chart.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <nlohmann/json.hpp>
#include <numbers>
#include <random>

namespace sdvx {
namespace {

constexpr auto inversionSource = "runtime/stage-inversion-source";
constexpr auto inversionColor = "runtime/stage-inversion-color";

bool hasSongBackground(const Selection& selection) {
    // Song ID 666 uses the same folder name as the authored background for song ID 1580.
    if ((selection.id == 271 && selection.chartKey != "exceed.json") ||
        (selection.id == 636 && selection.chartKey != "ultimate.json")) return false;
    return selection.id != 666 && (selection.id != 642 || selection.chartKey == "exceed.json")
        && hasAsset(selection.presentation("background/afp-scene.json"));
}

const nlohmann::json& backgroundCatalog() {
    static const auto catalog = nlohmann::json::parse(readAsset("stage_bg/catalog.json"));
    return catalog;
}

const nlohmann::json& originalStageRegistration() {
    static const auto table = nlohmann::json::parse(
#include "stage_registration_2026101300.inc"
    );
    return table;
}

const nlohmann::json& originalStageRegistration(const Selection& selection, bool dedicated) {
    const auto& table = originalStageRegistration();
    if (dedicated) {
        for (const auto& route : table.at("special_routes")) {
            if (route.at("song_id") == selection.id &&
                route.at("chart") == selection.chartKey)
                return table.at("special").at(std::to_string(route.at("key").get<int>()));
        }
    }
    const auto& ordinary = table.at("ordinary");
    const auto found = ordinary.find(std::to_string(selection.background));
    static const nlohmann::json empty = nlohmann::json::object();
    return found == ordinary.end() ? empty : *found;
}

void startAfpMovie(AfpMovie& movie, const nlohmann::json& label, double time) {
    if (label.is_string()) movie.start(label.get_ref<const std::string&>(), time);
    else movie.start(label.get<size_t>(), time);
}

struct TunnelCamera {
    std::optional<Vec2> center;
    float pitch, autoTilt, roll;
};

struct BackgroundTunnel {
    std::string normal, clear;
    float angle, speed;
    int sides;

    void draw(Render& render, double distance, const TunnelCamera& camera, bool cleared, float opacity = 1) const {
        // 1804bb960 builds complete strips, then translates the mesh before
        // rotating it around z = 1000. UVs and alpha stay attached to the rings.
        constexpr float radians = std::numbers::pi_v<float> / 180;
        constexpr float radius = 20, step = 500, pivot = 1000;
        constexpr float focal = 2317.640625f; // 960 * cot(0.7854 / 2)
        const auto center = *camera.center;
        const float phase = static_cast<float>(std::fmod(-3 * speed * distance, step));
        const float rotation = angle * radians + camera.roll;
        const float pitch = -.03f * (camera.pitch - 41.28f * radians);
        const float yaw = -.015f * std::clamp(camera.autoTilt, -20 * radians, 20 * radians);
        const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);
        const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
        const auto vertex = [&](int corner, float ring, float v) -> Render::TunnelVertex {
            const float theta = rotation + (corner % sides) * (2 * std::numbers::pi_v<float> / sides);
            const float x = radius * std::cos(theta), y = radius * std::sin(theta);
            const float z = ring + phase - pivot;
            const float viewY = y * cosPitch - z * sinPitch;
            const float tiltedZ = y * sinPitch + z * cosPitch;
            const float viewX = x * cosYaw + tiltedZ * sinYaw;
            const float depth = pivot + tiltedZ * cosYaw - x * sinYaw;
            const float alpha = std::round(255 * std::clamp(1 - (ring + phase) / 800, 0.f, 1.f)) / 255;
            return {{center.x * depth - focal * viewX, center.y * depth - focal * viewY, depth},
                    {float(corner) / sides, v}, alpha};
        };
        // The final ring is transparent at every phase of the 500-unit strip.
        std::array<Render::TunnelVertex, 3 * 8 * 6> mesh;
        size_t count = 0;
        for (int ring = 0; ring < 3; ++ring) {
            for (int side = 0; side < sides; ++side) {
                const auto a = vertex(side, ring * step, 0), b = vertex(side + 1, ring * step, 0);
                const auto c = vertex(side, (ring + 1) * step, 1), d = vertex(side + 1, (ring + 1) * step, 1);
                for (const auto& point : {a, c, b, b, c, d}) mesh[count++] = point;
            }
        }
        render.tunnelMesh(cleared ? clear : normal, std::span{mesh}.first(count), opacity);
    }
};

struct TunnelTransition {
    enum Mode { Cut, Noise, Fade };
    size_t from = 0, to = 0;
    Mode mode = Cut;
    double start = 0, end = 0;

    auto noiseRows(double time, bool incoming, float progress) const {
        // 1804ba6d0 / 180394640: independent strengths and random moving bands.
        // Generate bands at 120 Hz from the chart clock so skipped draw frames
        // and redraws do not change their lifetime or drift.
        using Row = std::array<uint8_t, 4>;
        std::array<Row, static_cast<size_t>(Height)> rows;
        const int strength = incoming ? std::min(32768, int(65536 * (1 - progress)))
                                      : std::min(4096, int(8192 * progress));
        rows.fill({0, uint8_t(strength >> 8), uint8_t(strength), 255});
        const double elapsed = time - start;
        const int tick = static_cast<int>(elapsed * .12);
        for (int born = std::max(0, tick - 96); born <= tick; ++born) {
            std::minstd_rand random(1 + static_cast<unsigned>(start) * 257 + born * 4099 + incoming * 65537);
            const auto roll = [&](int low, int high) {
                return std::uniform_int_distribution<int>(low, high)(random);
            };
            if (roll(0, 100) >= (incoming ? 40 : 90)) continue;
            const int y = roll(1, rows.size());
            const int height = roll(1, rows.size() / 8);
            const int lifetime = roll(100, 800);
            const double age = elapsed - born / .12;
            if (age > lifetime) continue;
            const int top = y + static_cast<int>(roll(-192, 192) * age / lifetime);
            const int amplitude = roll(128, 9182);
            const Row value{uint8_t(roll(0, 255)), uint8_t(amplitude >> 8), uint8_t(amplitude), 255};
            std::fill(rows.begin() + std::clamp(top, 0, int(rows.size())),
                      rows.begin() + std::clamp(top + height, 0, int(rows.size())), value);
        }
        return rows;
    }
};

} // namespace

std::string commonStageBackground(int number) {
    return "stage_bg/" + std::to_string(backgroundCatalog().at("backgrounds").value(std::to_string(number), 2));
}

std::string stageBackgroundFolder(const Selection& selection) {
    return hasSongBackground(selection) ? selection.presentation("background") : commonStageBackground(selection.background);
}

struct StageScene::Source {
    std::string folder;
    nlohmann::json scene, afp;
    std::map<std::pair<std::string, std::string>, AfpMovie> movies;

    auto movieKey(const nlohmann::json& entry) const {
        return std::pair{entry.value("folder", folder), entry.at("name").get<std::string>()};
    }
};

std::shared_ptr<const StageScene::Source> StageScene::load(const Selection& selection) {
    auto source = std::make_shared<Source>();
    source->folder = stageBackgroundFolder(selection);
    if (source->folder == selection.presentation("background"))
        source->scene = nlohmann::json::parse(readAsset(source->folder + "/scene.json"));
    source->afp = nlohmann::json::parse(readAsset(source->folder + "/afp-scene.json"));
    for (const auto& entry : source->afp.at("movies")) {
        const auto key = source->movieKey(entry);
        source->movies.try_emplace(key, key.first, key.second);
    }
    return source;
}

void StageScene::queueSourceImages(Render& render, const Source& source) {
    for (const auto& [_, movie] : source.movies) movie.queueTextures(render, true);
}

struct StageScene::Impl {
    Render& render;
    std::string background;
    bool specialBackground;
    bool deferTextures;
    bool backgroundClear = false;
    std::optional<AfpMovie> commonMovie;
    std::optional<AfpMovie> commonUpper;
    std::string commonUpperName;
    std::array<std::string, 2> commonGaugeLabels;
    nlohmann::json scene;
    enum class Pass { Background, AfterModels, Foreground };
    struct Layer {
        Rect bounds;
        Pass pass;
    };
    std::vector<Layer> sceneLayers;
    std::deque<AfpMovie> afpMovies;
    struct MovieSlot {
        std::unordered_map<std::string, AfpMovie*> movies;
        AfpMovie* active = nullptr;
    };
    enum class DrawKind { Movie, Model, Tunnel, Monitor };
    struct DrawLayer {
        int priority;
        DrawKind kind;
        size_t slot;
    };
    std::vector<DrawLayer> originalLayers;
    std::vector<int> priorities;
    std::vector<StagePostPass> originalPostPasses;
    std::unordered_map<size_t, MovieSlot> afpSlots;
    nlohmann::json afpEvents;
    nlohmann::json afpParameters;
    nlohmann::json afpGauges;
    struct GaugeChild { size_t slot; std::string name; };
    std::vector<GaugeChild> gaugeChildren;
    std::unordered_map<std::string, unsigned> afpOccurrences;
    std::optional<size_t> afpCutline;
    AfpMovie* afpMonitor = nullptr;
    std::optional<std::array<size_t, 2>> inversionLayers;
    std::array<bool, 2> inversionStarted{};
    bool extendedNoteDepth = false;
    Bindings sceneBindings;
    Bindings monitorBindings;
    std::vector<std::optional<BackgroundTunnel>> tunnels;
    TunnelTransition tunnelTransition;
    std::optional<size_t> filteredLayer;
    struct FilterStage {
        std::string texture;
        std::vector<ImageBinding*> bindings;
    };
    std::vector<FilterStage> filterStages;
    std::vector<ImageBinding*> rawBindings;
    bool tunnelNoise = false;
    // The wall clock starts during READY and continues through hidden SIDEBG slots.
    double tunnelClock = 0, tunnelDistance = 0, tunnelRate = .1;
    std::deque<StageModel> sceneModels;
    std::vector<std::vector<size_t>> modelGroups;

    Impl(Render& graphics, const Selection& selection, const Source& source, bool defer)
        : render(graphics), background(source.folder),
          specialBackground(background == selection.presentation("background")), deferTextures(defer) {
        const auto addTunnel = [&](const nlohmann::json& entry) {
            if (entry.is_null()) {
                tunnels.emplace_back();
                return;
            }
            const auto prefix = "stage_bg/tunnel/tex_" + entry[0].get<std::string>();
            tunnels.emplace_back(BackgroundTunnel{
                prefix + "_no.png", prefix + "_cl.png", entry[1], entry[2], entry[3]});
        };
        if (specialBackground) {
            scene = source.scene;
            for (const auto& tunnel : scene.value("tunnels", nlohmann::json::array())) addTunnel(tunnel);
            if (const auto layers = scene.find("inversion_layers"); layers != scene.end()) {
                inversionLayers = layers->get<std::array<size_t, 2>>();
                sceneBindings.emplace("nomal_usr", ImageBinding{inversionSource, {0, Height, Width, -Height}, {0, 0, Width, Height}});
                sceneBindings.emplace("gold_usr", ImageBinding{inversionColor, {0, Height, Width, -Height}, {0, 0, Width, Height}});
            }
            for (const auto& name : scene.value("hide", std::vector<std::string>{})) sceneBindings[name] = "";
            for (const auto& layer : scene.at("initial")) {
                const auto bounds = layer.value("bounds", std::array{0.f, 0.f, Width, Height});
                sceneLayers.push_back({{bounds[0], bounds[1], bounds[2], bounds[3]},
                    layer.value("foreground", false) ? Pass::Foreground :
                    layer.value("after_models", false) ? Pass::AfterModels : Pass::Background});
            }
            {
                const auto& afp = source.afp;
                afpEvents = afp.at("events");
                for (const auto& [key, actions] : afpEvents.items())
                    if (std::ranges::any_of(actions, [](const auto& action) { return action.contains("occurrence"); }))
                        afpOccurrences.emplace(key, 0);
                afpParameters = afp.value("parameters", nlohmann::json::object());
                afpGauges = afp.value("gauges", nlohmann::json::array());
                for (const auto& entry : afp.at("movies")) {
                    const auto slot = entry.at("slot").get<size_t>();
                    const auto& name = entry.at("name").get_ref<const std::string&>();
                    auto& movie = afpMovies.emplace_back(source.movies.at(source.movieKey(entry)).fork());
                    movie.configure(entry);
                    auto& group = afpSlots[slot];
                    group.movies.emplace(name, &movie);
                    if (!group.active) group.active = &movie;
                    if (const auto label = entry.find("label"); label != entry.end()) {
                        startAfpMovie(movie, *label, 0);
                        group.active = &movie;
                    }
                }
                if (const auto slot = afp.find("cutline_slot"); slot != afp.end())
                    afpCutline = slot->get<size_t>();
                if (afpCutline && *afpCutline == sceneLayers.size())
                    sceneLayers.push_back({{0, 0, Width, Height}, Pass::AfterModels});
                if (const auto monitor = afp.find("monitor"); monitor != afp.end()) {
                    afpMonitor = afpSlots.at(monitor->at("slot").get<size_t>()).movies.at(
                        monitor->at("name").get_ref<const std::string&>());
                }
            }
            for (const auto& model : scene.value("models", nlohmann::json::array())) {
                const auto slot = model.value("slot", sceneModels.size());
                if (slot >= modelGroups.size()) modelGroups.resize(slot + 1);
                modelGroups[slot].push_back(sceneModels.size());
                sceneModels.emplace_back(background + "/models/" + model.at("asset").get<std::string>(),
                    model.at("placement").get<std::array<float, 4>>(), deferTextures);
            }
        }
        if (!specialBackground) {
            const auto& config = source.afp;
            const auto& movies = config.at("movies");
            const auto& entry = movies.front();
            if (const auto gauge = config.find("gauge"); gauge != config.end())
                commonGaugeLabels = {gauge->at("normal"), gauge->at("clear")};
            auto& movie = commonMovie.emplace(source.movies.at(source.movieKey(entry)).fork());
            startAfpMovie(movie, entry.at("label"), 0);
            if (movies.size() > 1) {
                const auto& upper = movies.at(1);
                commonUpperName = upper.at("name").get<std::string>();
                auto& layer = commonUpper.emplace(source.movies.at(source.movieKey(upper)).fork());
                startAfpMovie(layer, upper.at("label"), 0);
            }
        }
        if (tunnels.empty()) {
            const auto& commonTunnels = backgroundCatalog().at("tunnels");
            if (const auto entry = commonTunnels.find(std::to_string(selection.background)); entry != commonTunnels.end())
                addTunnel(*entry);
        }
        if (!tunnels.empty()) tunnelClock = std::floor(applicationTime());
        if (specialBackground && selection.id == 1580)
            sceneModels.emplace_back("crew/kureha/model/kureha_9thkac_0101.model3.json",
                                     std::array{540.f, 800.f, .68f, .68f}, deferTextures);
        loadOriginalLayers(selection);
    }

    void loadOriginalLayers(const Selection& selection) {
        const auto& registration = originalStageRegistration(selection, specialBackground);
        const auto registrations = registration.value("post", nlohmann::json::array());
        for (const auto& pass : registrations) {
            const auto range = pass.at("input_priorities").get<std::array<int, 2>>();
            using Output = StagePostPass::Output;
            const auto& captures = pass.at("captures");
            originalPostPasses.push_back({range[0], range[1], pass.at("present_priority"),
                pass.at("types").get<std::vector<int>>(), captures.empty() ? Output::Filtered :
                inversionLayers ? Output::Inversion : Output::Movie,
                inversionLayers && range[0] < 64 ? 3 : 2});
            if (inversionLayers || captures.empty()) continue;
            const auto& config = captures.front();
            const auto& name = config.at("movie").get_ref<const std::string&>();
            const auto slot = std::ranges::find_if(afpSlots, [&](const auto& entry) {
                return entry.second.movies.contains(name);
            });
            if (slot == afpSlots.end()) continue;
            filteredLayer = slot->first;
            const auto bind = [&](const nlohmann::json& target) {
                auto& binding = sceneBindings[target.get_ref<const std::string&>()];
                binding.sourceRect = {0, Height, Width, -Height};
                return &binding;
            };
            for (const auto& target : config.at("raw_targets")) rawBindings.push_back(bind(target));
            for (size_t step = 0; step < pass.at("types").size(); ++step)
                filterStages.push_back({"runtime/stage-composite-filter/" + std::to_string(step), {}});
            for (const auto& target : config.at("effect_targets"))
                filterStages.at(target.at("stage").get<size_t>()).bindings.push_back(bind(target.at("target")));
        }
        const auto base = registration.value("base", nlohmann::json{{"clip", nullptr}, {"priority", nullptr}});
        const auto afp = registration.value("afp", nlohmann::json::array());
        const auto models = registration.value("live2d", nlohmann::json::array());
        const auto add = [&](int priority, DrawKind kind, size_t slot) {
            originalLayers.push_back({priority, kind, slot});
        };
        if (commonMovie) add(base.at("priority").is_null() ? 64 : base.at("priority").get<int>(), DrawKind::Movie, 0);
        if (commonUpper) {
            const auto upper = std::ranges::find_if(afp, [&](const auto& entry) { return entry.at("clip") == commonUpperName; });
            add(upper == afp.end() ? 65 : upper->at("priority").get<int>(), DrawKind::Movie, 1);
        }
        for (size_t slot = 0; slot < sceneLayers.size(); ++slot) {
            if (afpSlots.find(slot) == afpSlots.end()) continue;
            const auto& group = afpSlots.at(slot);
            const auto& initial = scene.at("initial");
            std::optional<int> priority;
            if (slot < initial.size() && initial[slot].contains("priority")) {
                priority = initial[slot].at("priority").get<int>();
            } else {
                const auto matches = [&](const auto& entry) {
                    return !entry.at("clip").is_null() &&
                        group.movies.contains(entry.at("clip").template get_ref<const std::string&>());
                };
                const auto found = std::ranges::find_if(afp, matches);
                if (const auto* native = found != afp.end() ? &*found : matches(base) ? &base : nullptr) {
                    priority = native->at("priority").get<int>();
                    for (const auto& child : native->at("gauge_children")) {
                        const auto& name = child.get_ref<const std::string&>();
                        if (!name.empty() && name != "/")
                            gaugeChildren.push_back({slot, name.starts_with('/') ? name.substr(1) : name});
                    }
                }
            }
            if (filteredLayer == slot ||
                (inversionLayers && (slot == (*inversionLayers)[0] || slot == (*inversionLayers)[1]))) continue;
            const int fallback = sceneLayers[slot].pass == Pass::Foreground ? 36 :
                sceneLayers[slot].pass == Pass::AfterModels ? 57 : 64;
            add(priority.value_or(fallback), DrawKind::Movie, slot);
        }
        const auto modelEntries = scene.find("models");
        const int defaultModelPriority = models.size() == 1 ? models.front().at("priority").get<int>() : 60;
        for (size_t slot = 0; slot < sceneModels.size(); ++slot)
            add(modelEntries == scene.end() ? defaultModelPriority :
                modelEntries->at(slot).value("priority", defaultModelPriority), DrawKind::Model, slot);
        if (!tunnels.empty()) add(64, DrawKind::Tunnel, 0);
        if (afpMonitor) add(57, DrawKind::Monitor, 0);
        std::stable_sort(originalLayers.begin(), originalLayers.end(), [](const auto& a, const auto& b) {
            return a.priority > b.priority;
        });
        for (const auto& layer : originalLayers)
            if (priorities.empty() || priorities.back() != layer.priority) priorities.push_back(layer.priority);
    }

    void queueInitial(std::span<const StageCue> cues) {
        tunnelNoise = !tunnels.empty() && std::ranges::any_of(cues, [](const StageCue& cue) {
            return cue.name == "SpecialN" && cue.parameters.size() >= 3 &&
                cue.parameters[0] == "SIDEBG" && cue.parameters[2] == "i1";
        });
        prepareCaptures();
    }

    void prepareCaptures() {
        if (tunnelNoise) render.prepareTunnelNoise();
        if (!originalPostPasses.empty()) {
            render.prepareStageLayer();
            size_t scratchCount = 0;
            for (size_t i = 0; i < originalPostPasses.size(); ++i) {
                const auto& pass = originalPostPasses[i];
                render.prepareCanvasCopy(StageCaptureTargets[i]);
                if (pass.output == StagePostPass::Output::Filtered) {
                    scratchCount = std::max(scratchCount, std::min(pass.effects.size(), StageFilterTargets.size()));
                }
            }
            for (size_t i = 0; i < scratchCount; ++i) render.prepareCanvasCopy(StageFilterTargets[i]);
            for (const auto& stage : filterStages) render.prepareCanvasCopy(stage.texture);
            if (inversionLayers) {
                render.prepareCanvasCopy(inversionColor);
                render.prepareStageColorConversion();
            }
        }
    }

    void dispatchAfp(std::string_view key, double time) {
        const auto route = afpEvents.find(key);
        if (route == afpEvents.end()) return;
        const auto counter = afpOccurrences.find(route.key());
        const unsigned occurrence = counter == afpOccurrences.end() ? 0 : ++counter->second;
        for (const auto& action : *route) {
            if (const auto number = action.find("occurrence"); number != action.end() &&
                number->get<unsigned>() != occurrence) continue;
            auto& slot = afpSlots.at(action.at("slot").get<size_t>());
            if (const auto name = action.find("name"); name != action.end()) {
                auto* selected = slot.movies.at(name->get_ref<const std::string&>());
                if (slot.active != selected) slot.active->stop();
                slot.active = selected;
            }
            auto& movie = *slot.active;
            if (const auto start = action.find("start"); start != action.end()) startAfpMovie(movie, *start, time);
            if (action.value("stop", false)) movie.stop();
            else if (const auto rate = action.find("rate"); rate != action.end())
                movie.setRate(rate->get<double>(), time);
            else if (action.value("restart", false)) startAfpMovie(movie, action.at("label"), time);
            else movie.cue(action.at("path").get_ref<const std::string&>(),
                           action.at("label").get_ref<const std::string&>(), time);
            if (inversionLayers) {
                const auto index = action.at("slot").get<size_t>();
                for (size_t target = 0; target < inversionLayers->size(); ++target)
                    if ((*inversionLayers)[target] == index) inversionStarted[target] = true;
            }
            for (const auto& name : action.value("hide", std::vector<std::string>{})) sceneBindings[name] = "";
            for (const auto& name : action.value("show", std::vector<std::string>{})) sceneBindings.erase(name);
        }
    }

    void cue(const StageCue& cue) {
        if (cue.name == "AutomaPhrase" && afpMonitor) {
            if (cue.parameters[0] != "0")
                monitorBindings["monitor_jacket"] = "songs/1438/" + cue.parameters[0] + "/jacket.png";
            afpMonitor->start(cue.parameters[1], cue.time);
        } else if (cue.name == "AutomaPanel" && afpMonitor) {
            afpMonitor->start(cue.parameters[0], cue.time);
        } else if (cue.name == "SetFrameLabel" && cue.parameters.size() >= 2) {
            dispatchAfp("SetFrameLabel:" + cue.parameters[0] + ":" + cue.parameters[1], cue.time);
        } else if (cue.name == "SpecialN" && cue.parameters.size() >= 2) {
            const auto& target = cue.parameters[0];
            const auto& event = cue.parameters[1];
            if (afpEvents.is_object() && (target == "EXBG" || (target == "EXMC_LABEL" && cue.parameters.size() >= 3))) {
                const auto key = target + ":" + event + (target == "EXMC_LABEL" ? ":" + cue.parameters[2] : "");
                dispatchAfp(key, cue.time);
            }
            if (target == "EXMC_PARAM" && cue.parameters.size() >= 4 && afpParameters.is_object()) {
                if (const auto route = afpParameters.find(event); route != afpParameters.end())
                    for (const auto& action : *route)
                        afpSlots.at(action.at("slot").get<size_t>()).active->parameter(
                            action.at("path").get_ref<const std::string&>(),
                            action.at("from").get_ref<const std::string&>(), action.at("to").get_ref<const std::string&>(),
                            cue.time, cue.end, std::stod(cue.parameters[2]), std::stod(cue.parameters[3]));
            }
            if (target == "SIDEBG" && cue.parameters.size() >= 3) {
                const auto& mode = cue.parameters[2];
                if (event.size() < 2 || event.front() != 'i' ||
                    (mode != "i0" && mode != "i1" && mode != "i2")) return;
                size_t index = 0;
                const auto [end, error] = std::from_chars(event.data() + 1, event.data() + event.size(), index);
                if (error == std::errc{} && end == event.data() + event.size() && index < tunnels.size())
                    tunnelTransition = {tunnelTransition.to, index, static_cast<TunnelTransition::Mode>(mode[1] - '0'),
                                        cue.time, cue.end};
            } else if (target == "RENDER_PRESET") {
                // NoteRenderer 1804b1e60 / 1804aebe0 uses preset 1 for the larger depth constant.
                extendedNoteDepth = event == "i1";
            } else if (target == "EXBGTEX" && scene.contains("text")) {
                const int index = std::stoi(event.substr(event.front() == 'i' ? 1 : 0));
                const auto& text = scene.at("text");
                char suffix[16];
                std::snprintf(suffix, sizeof(suffix), "%02d.png", index);
                const auto path = background + "/" + text.at("prefix").get<std::string>() + suffix;
                const auto size = render.imageSize(path);
                sceneBindings[text.at("slot").get<std::string>()] = ImageBinding{path, {}, {0,0,size.x,size.y}};
            } else if (target == "LIVE2D" && !sceneModels.empty()) {
                if (const auto motions = scene.find("motions"); motions != scene.end()) {
                    size_t index = 0;
                    if (cue.parameters.size() > 2) {
                        std::string_view value = cue.parameters[2];
                        if (value.starts_with('i')) value.remove_prefix(1);
                        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), index);
                        if (error != std::errc{} || end != value.data() + value.size()) return;
                    }
                    if (index >= modelGroups.size()) return;
                    for (const auto model : modelGroups[index]) {
                        if (event == "LIVE2D_OFF") {
                            sceneModels[model].play({}, {}, cue.time);
                        } else {
                            const auto& config = scene.at("models").at(model);
                            const auto& modelMotions = config.contains("motions") ? config.at("motions") : *motions;
                            if (const auto motion = modelMotions.find(event); motion != modelMotions.end()) {
                                sceneModels[model].play((*motion)[0].get_ref<const std::string&>(),
                                                        (*motion)[1].get_ref<const std::string&>(), cue.time);
                                if (const auto placements = config.find("motion_placements"); placements != config.end()) {
                                    if (const auto placement = placements->find(event); placement != placements->end())
                                        sceneModels[model].setPlacement(placement->get<std::array<float, 4>>());
                                }
                            }
                        }
                    }
                } else {
                    auto& model = sceneModels.front();
                    if (event == "LIVE2D_KUREHA_IN" || event == "i9") model.play("In", "Idle", cue.time);
                    else if (event == "LIVE2D_KUREHA_KEN" || event == "i10") model.play("Ken", "Idle2", cue.time);
                    else if (event == "LIVE2D_KUREHA_LAST" || event == "i11") model.play("Last", "Idle3", cue.time);
                    else if (event == "LIVE2D_OUT" || event == "LIVE2D_KUREHA_OUT" || event == "i5")
                        model.play("Out", {}, cue.time);
                    else if (event == "LIVE2D_OFF" || event == "i0") model.play({}, {}, cue.time);
                }
            }
        }
    }

    void drawLayer(size_t slot) {
        const auto movie = afpSlots.find(slot);
        if (movie == afpSlots.end() || !movie->second.active->started()) return;
        movie->second.active->draw(render, sceneLayers[slot].bounds, sceneBindings);
    }

    void drawTunnels(double time, const TunnelCamera& camera) {
        if (tunnels.empty() || !camera.center) return;
        const auto& change = tunnelTransition;
        const auto& current = tunnels[change.to];
        if (change.mode == TunnelTransition::Cut || time >= change.end) {
            if (current) current->draw(render, tunnelDistance, camera, backgroundClear);
            return;
        }
        const auto& previous = tunnels[change.from];
        const float progress = static_cast<float>(std::clamp((time - change.start) / (change.end - change.start), 0., 1.));
        if (change.mode == TunnelTransition::Fade) {
            if (previous)
                previous->draw(render, tunnelDistance, camera, backgroundClear, int(255 * (1 - progress)) / 255.f);
            if (current && progress > 0)
                current->draw(render, tunnelDistance, camera, backgroundClear, int(255 * progress) / 255.f);
        } else {
            const auto drawNoise = [&](const BackgroundTunnel& tunnel, bool incoming) {
                const auto rows = change.noiseRows(time, incoming, progress);
                render.drawTunnelNoise(std::as_bytes(std::span{rows}), int(time * .12) % 4096, [&] {
                    tunnel.draw(render, tunnelDistance, camera, backgroundClear);
                });
            };
            if (previous) drawNoise(*previous, false);
            if (current) drawNoise(*current, true);
        }
    }

    void drawCommonBase(const TunnelCamera& camera) {
        if (background == "stage_bg/2") {
            // 180486b31 and 1804bd503 set the 40-unit pivot offset and half tilt.
            constexpr float pivotX = Width * .5f, pivotY = 720;
            const float angle = camera.roll - camera.autoTilt * .5f;
            const float c = std::cos(angle), s = std::sin(angle);
            render.pushTransform({c, -s, pivotX * (1 - c) + pivotY * s,
                                  s,  c, pivotY * (1 - c) - pivotX * s});
            commonMovie->draw(render, {0, 0, Width, Height}, {});
            render.popTransform();
        } else commonMovie->draw(render, {0, 0, Width, Height}, {});
    }

    void drawAtPriorities(double time, const TunnelCamera& camera, int first, int end) {
        for (const auto& layer : originalLayers) {
            if (layer.priority < first || layer.priority >= end) continue;
            switch (layer.kind) {
            case DrawKind::Movie:
                if (commonMovie) {
                    if (layer.slot == 0) drawCommonBase(camera);
                    else commonUpper->draw(render, {0, 0, Width, Height}, {});
                } else drawLayer(layer.slot);
                break;
            case DrawKind::Model: sceneModels[layer.slot].draw(render); break;
            case DrawKind::Tunnel: drawTunnels(time, camera); break;
            case DrawKind::Monitor: afpMonitor->draw(render, {0, 0, Width, Height}, monitorBindings); break;
            }
        }
    }

    void setGauge(double time, bool clear) {
        if (clear == backgroundClear) return;
        backgroundClear = clear;
        // The original gauge event sets a frame label on the current clip (1804bfb7f).
        if (!specialBackground && !commonGaugeLabels[clear].empty())
            commonMovie->cue("/", commonGaugeLabels[clear], time, true);
        for (const auto& gauge : afpGauges)
            afpSlots.at(gauge.at("slot").get<size_t>()).active->cue(
                "/", gauge.at(clear ? "clear" : "normal").get_ref<const std::string&>(), time, true);
        for (const auto& child : gaugeChildren)
            afpSlots.at(child.slot).active->cue(child.name, clear ? "clear" : "normal", time, true);
    }

    void queueGraphics(std::span<const StageCue> cues) {
        const auto image = [&](std::string_view path) {
            if (deferTextures) render.queueImage(path, true);
            else render.imageSize(path);
        };
        for (const auto& tunnel : tunnels) if (tunnel) {
            image(tunnel->normal);
            image(tunnel->clear);
        }
        if (afpMonitor) for (const auto& cue : cues)
            if (cue.name == "AutomaPhrase" && cue.parameters[0] != "0")
                image("songs/1438/" + cue.parameters[0] + "/jacket.png");
        if (commonMovie) commonMovie->queueTextures(render, deferTextures, deferTextures);
        if (commonUpper) commonUpper->queueTextures(render, deferTextures, deferTextures);
        for (const auto& movie : afpMovies) movie.queueTextures(render, deferTextures, deferTextures);
    }
};
StageScene::StageScene(Render& render, const Selection& selection, const Source& source, bool deferTextures)
    : impl_(std::make_unique<Impl>(render, selection, source, deferTextures)) {}
StageScene::~StageScene() = default;

void StageScene::queueInitial(std::span<const StageCue> cues) { impl_->queueInitial(cues); }
void StageScene::cue(const StageCue& cue) { impl_->cue(cue); }
void StageScene::setGauge(double time, bool clear) { impl_->setGauge(time, clear); }
void StageScene::update(double time, double now, double bpm) {
    if (!impl_->tunnels.empty()) {
        // 1804bb520 starts at 100 units/s. 1804bf380 integrates wall time,
        // and the tempo event at 1804bf784 switches to 2 units/s per BPM.
        const double clock = std::floor(now);
        impl_->tunnelDistance += (clock - impl_->tunnelClock) * impl_->tunnelRate;
        impl_->tunnelClock = clock;
        if (bpm > 0) impl_->tunnelRate = bpm * .002;
    }
    for (auto& model : impl_->sceneModels) model.update(time);
    if (impl_->commonMovie) impl_->commonMovie->advance(time);
    if (impl_->commonUpper) impl_->commonUpper->advance(time);
    for (auto& [slot, group] : impl_->afpSlots) group.active->advance(time);
}
void StageScene::queueGraphics(std::span<const StageCue> cues) { impl_->queueGraphics(cues); }
bool StageScene::prepareGraphics() {
    bool ready = true;
    for (auto& model : impl_->sceneModels)
        if (!model.prepareGraphics()) ready = false;
    return ready;
}
void StageScene::releaseGraphics() {
    for (auto& model : impl_->sceneModels) model.releaseGraphics();
}
void StageScene::restoreGraphics() {
    for (auto& model : impl_->sceneModels) model.restoreGraphics();
    impl_->prepareCaptures();
}
std::span<const StagePostPass> StageScene::postPasses() const { return impl_->originalPostPasses; }
void StageScene::drawAtPriorities(double time, std::optional<Vec2> center, float pitch, float autoTilt, float roll,
                                  int first, int end) {
    impl_->drawAtPriorities(time, {center, pitch, autoTilt, roll}, first, end);
}
std::span<const int> StageScene::drawPriorities() const { return impl_->priorities; }
bool StageScene::postMovieActive(size_t index) const {
    if (impl_->inversionLayers) return impl_->inversionStarted.at(index);
    return impl_->filteredLayer && impl_->afpSlots.at(*impl_->filteredLayer).active->started();
}
void StageScene::drawPostInversion(size_t index, std::string_view source) {
    impl_->sceneBindings.at("nomal_usr").path = source;
    impl_->render.convertStageColor(source, inversionColor, impl_->originalPostPasses.at(index).defaultMode, 100);
    impl_->drawLayer((*impl_->inversionLayers)[index]);
}
void StageScene::drawPostMovie(std::string_view source,
                              const std::function<bool(size_t, std::string_view, std::string_view)>& filter) {
    const auto slot = *impl_->filteredLayer;
    auto& movie = *impl_->afpSlots.at(slot).active;
    for (auto* binding : impl_->rawBindings) binding->path = source;
    for (size_t step = 0; step < impl_->filterStages.size(); ++step) {
        const auto& stage = impl_->filterStages[step];
        // An inactive filter produces the same image as its input. Bind that
        // input to the AFP slots directly, including intermediate results.
        if (filter(step, source, stage.texture)) source = stage.texture;
        for (auto* binding : stage.bindings) binding->path = source;
    }
    movie.draw(impl_->render, impl_->sceneLayers[slot].bounds, impl_->sceneBindings);
}
bool StageScene::extendedNoteDepth() const { return impl_->extendedNoteDepth; }

}
