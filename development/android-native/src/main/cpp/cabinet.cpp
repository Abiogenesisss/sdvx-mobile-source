#include "cabinet.h"
#include "stage_automation.h"
#include "cabinet_crew_catalog.h"
#include "cabinet_rewards.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <RmlUi/Core.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace sdvx {
std::string localCardState();
std::future<bool> submitLocalPin(std::string pin);
std::string normalizeSearch(std::string_view value);
int64_t localDay();

namespace {
using Json = nlohmann::json;
int64_t unixTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
constexpr std::array<int, ButtonCount> DefaultKeys{
    SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_J, SDL_SCANCODE_K,
    SDL_SCANCODE_C, SDL_SCANCODE_M, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F1
};
constexpr double MatchTime = 10000;
constexpr double EntryConfirmHold = 180000. / 120, PaymentNoticeTime = 600000. / 120;
// In the NABLA recording, entry13 starts at 74.052 s and the connect window closes at 74.482 s.
constexpr double EntryLoadingVoiceHold = 430;
constexpr double PaymentReceiptSkipTime = 1001, PaymentReceiptTime = 3001;
// Use the original state counters 10 and 352 on the mobile 120 fps AFP clock.
constexpr double CautionOpenSoundAt = 10000. / 120, CautionCloseSoundAt = 352000. / 120;
// 18030c270 enters the wait state, then waits until the counter exceeds 20.
constexpr double SongPreviewDelay = 22000. / 120;
constexpr double TitleCallAt = 240000. / 120;
constexpr auto SongSelectMusic = "menu/audio/bgm_ver7_03_musicselect.wav";
constexpr auto EntryOpenLow = "entry_original/audio/se_02_in001_entry_low.wav";
constexpr auto EntryCloseLow = "entry_original/audio/se_03_out001_entry_low.wav";
constexpr auto DifficultyOpen = "selection_ui/audio/se_02_in003_difficulty.wav";
constexpr auto DifficultyClose = "selection_ui/audio/se_03_out004_difficulty.wav";
constexpr auto EntryOpenHigh = "entry_original/audio/se_02_in002_entry_high.wav";
constexpr auto EntryCloseHigh = "entry_original/audio/se_03_out002_entry_high.wav";
constexpr auto PinInput = "entry_original/audio/s12_card_entry_num_input.wav";
constexpr auto NameDecide = "entry_original/audio/s21_name_entry_decide.wav";
constexpr auto NameCursor = "entry_original/audio/s22_name_entry_cursor.wav";
constexpr auto NameCancel = "entry_original/audio/s23_name_entry_cancel.wav";
constexpr auto NameEnd = "entry_original/audio/s24_name_entry_end.wav";
constexpr auto CountMove = "result_force/audio/s151_result_score_move.wav";
constexpr auto CountEnd = "result_force/audio/s152_result_score_decide.wav";
constexpr auto SkillTitleCursor = "skill_effects/audio/title-cursor.wav";
constexpr auto ShutterIn = "flow/se_02_in006_shutter_nabra.wav";
constexpr auto ShutterOut = "flow/se_03_out005_shutter_nabra.wav";
constexpr auto TrackIn = "flow/se_02_in004_musicselect_shutter.wav";
constexpr auto TrackOut = "flow/se_03_out003_musicselect_shutter.wav";
constexpr auto KonamiSound = "startup/sd_sram04_logo_konami.wav";
// NABLA 2026101300, 1805ee380. Every predecessor must be unlocked at this difficulty or higher.
constexpr std::array ExChallengeSongs{2418, 2417, 2416, 2415, 2419, 2413};
constexpr int MaxBlasterEnergy = 10000;
constexpr int GeneratorBatchLimit = 5;

// 18064a400 applies the 2DX sample's attenuation before mixing.
constexpr float twoDxGain(int attenuation) { return (128 - attenuation) / 127.f; }

int wrap(int value, int size) { return ((value % size) + size) % size; }

std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

std::string keyFor(const Selection& track) {
    // Keep existing local scores when switching the asset layout to original HDD names.
    return (track.id == 666 ? "id-666" : std::to_string(track.id == 1580 ? 666 : track.id)) + "/" + track.chartKey;
}

int lampRank(std::string_view outcome) {
    constexpr std::array<std::string_view, 8> lamps{"", "crash", "comp", "comp_ex", "comp_ultra", "uc", "puc", "spuc"};
    const auto found = std::find(lamps.begin(), lamps.end(), outcome);
    return found == lamps.end() ? 0 : static_cast<int>(found - lamps.begin());
}
}

Cabinet::Cabinet(Render& render, Audio& audio, bool showStartup)
    : render_(render), audio_(audio), navigator_(render, audio), keyMap_(DefaultKeys) {
    if (showStartup) {
        startup_.emplace();
        return;
    }
    loadCatalogs();
    prepareProfile();
    render_.prefetchAfp("title_original", "title", "in", true);
    openDocument();
    now_ = SDL_GetTicksNS() / 1000000.0;
    enter(Page::Title);
}

void Cabinet::loadCatalogs() {
    const auto started = std::chrono::steady_clock::now();
    const auto catalog = Json::parse(readAsset("songs/library.json"));
    const auto selectionData = Json::parse(readAsset("songs/selection.json"));
    floorContests_ = selectionData.at("floor_contests").get<std::vector<std::string>>();
    const auto details = Json::parse(readAsset("songs/details.json"), nullptr, false);
    versions_.push_back(0);
    const auto externalFolders = externalMusicFolders();
    for (const auto& entry : catalog) {
        Song song{}; song.version=entry.at("version").get<int>();
        song.searchKey = entry.value("search_key", lower(entry.at("title").get<std::string>() + "\n" + entry.at("artist").get<std::string>()));
        const int id = entry.at("id").get<int>();
        const auto challenge = std::find(ExChallengeSongs.begin(), ExChallengeSongs.end(), id);
        if (challenge != ExChallengeSongs.end()) song.challenge = int(challenge - ExChallengeSongs.begin());
        const auto& meta = selectionData.at(std::to_string(id));
        song.titleKey=meta.at("title_key"); song.artistKey=meta.at("artist_key");
        song.date=meta.at("date"); song.genre=meta.at("genre"); song.contests=meta.at("contests").get<std::vector<int>>();
        song.kana=meta.at("kana"); song.beginner=meta.at("beginner");
        const auto& rows = entry.at("charts");
        const auto folder = entry.at("folder").get<std::string>();
        const auto prefix = "music/" + folder + "/";
        const bool externalFolder = externalFolders.contains(folder);
        if (!externalFolder && !std::ranges::any_of(rows, [](const auto& row) { return row.value("bundled", false); })) continue;
        const auto shared = entry.at("music").get<std::string>();
        const auto preview = entry.at("preview").get<std::string>();
        const bool generated = id == AutomationParadise2019;
        const bool arrangementInstalled = generated && hasAsset("songs/1438/arrangement.json");
        for (const auto& row : rows) {
            Chart chart;
            chart.track = {
                folder, row.at("chart"), row.at("jacket"), entry.at("title"), entry.at("artist"),
                row.at("name"), row.at("label"), row.at("effect"), row.at("illustrator"),
                id, row.at("level"), entry.at("bpm_min"), entry.at("bpm_max"), entry.at("background")
            };
            auto& track = chart.track;
            track.chartKey = lower(track.difficulty) + ".json";
            track.maxExscore = row.value("max_exscore", 0);
            const bool external = !row.value("bundled", false);
            if (external && !externalFolder) continue;
            const auto exists = external ? hasExternalAsset : hasAsset;
            if (!(generated ? arrangementInstalled : exists(track.asset(track.chart)))) continue;
            track.music = shared;
            if (!generated) {
                // Older catalogs may omit the difficulty-specific HDD arrangement.
                auto arrangement = row.value("music", track.chart.substr(0, track.chart.rfind('.')) + ".s3v");
                if (exists(track.asset(arrangement))) track.music = std::move(arrangement);
                else if (row.contains("music") || !exists(track.asset(shared))) continue;
            }
            // External imports may contain just the normal-size jacket, or the shared slot-1 image.
            const auto paddedId = folder.substr(0, folder.find('_'));
            const auto cover = "jk_" + paddedId + "_";
            const auto slot = std::to_string(row.at("slot").get<int>());
            const std::array jackets{track.jacket,cover+slot+".png",cover+"1_b.png",cover+"1.png",
                                     cover+slot+"_s.png",cover+"1_s.png"};
            const auto found = std::ranges::find_if(jackets, [&](const auto& file) { return exists(prefix+file); });
            if (found == jackets.end()) continue;
            track.jacket = *found;
            track.externalChart = track.externalMusic = track.externalPreview = external;
            track.preview = exists(prefix + preview) ? preview : "";
            chart.installed = true;
            chart.recordKey = keyFor(chart.track);
            if (details.is_object()) {
                const auto item = details.find(std::to_string(id));
                if (item != details.end()) {
                    const auto& charts = item->at("charts");
                    const auto info = charts.find(lower(chart.track.difficulty));
                    if (info != charts.end()) {
                        const auto& values = info->at("radar");
                        constexpr std::array axes{"notes", "peak", "tsumami", "tricky", "hand-trip", "one-hand"};
                        for (size_t i = 0; i < axes.size(); ++i)
                            chart.radar[i] = values.value(axes[i], 0);
                    }
                }
            }
            song.charts.push_back(std::move(chart));
        }
        if (!song.charts.empty()) {
            versions_.push_back(song.version);
            songs_.push_back(std::move(song));
        }
    }
    if (songs_.empty()) throw std::runtime_error("NABLA song library contains no playable charts");
    std::sort(versions_.begin(), versions_.end());
    versions_.erase(std::unique(versions_.begin(), versions_.end()), versions_.end());
    const auto courseSets=Json::parse(readAsset("skill_selector/courses.json"));
    for (const auto& set:courseSets) for (const auto& source:set.at("courses")) {
        Course course{source.at("name"),set.at("name"),source.at("level"),source.at("nameID"),
                      set.at("id"),source.at("id"),{}};
        for (const auto& item:source.at("tracks")) {
            const int id=item.at("mid"), type=item.at("mty");
            for (int s=0;s<int(songs_.size());++s) if (songs_[s].charts.front().track.id==id) {
                for (int c=0;c<int(songs_[s].charts.size());++c) {
                    const auto& chart=songs_[s].charts[c];
                    const auto& label = chart.track.label;
                    if ((type==0 && label=="NOV") || (type==1 && label=="ADV") ||
                        (type==2 && label=="EXH") || (type==4 && label=="MXM") ||
                        (type==3 && label!="NOV" && label!="ADV" && label!="EXH" && label!="MXM" && label!="ULT")) {
                        course.tracks.emplace_back(s,c);break;
                    }
                }
                break;
            }
        }
        if (course.tracks.empty() || course.tracks.size()!=source.at("tracks").size()) continue;
        courses_.push_back(std::move(course));
    }
    options_.speed = 7;
    const auto customization = Json::parse(readAsset("subscreen/catalog.json"));
    const auto& cards = customization.at("cards");
    appealCard_ = int(std::find_if(cards.begin(), cards.end(), [](const Json& card) {
        return card.at("default").get<bool>();
    }) - cards.begin());
    for (const auto& card : cards) {
        const auto uv = card.at("thumbnail_uv").get<std::array<float, 4>>();
        appealCards_.push_back({card.at("id"), card.at("image"), card.at("thumbnail"), card.at("name"), card.at("illustrator"),
            {uv[0], uv[1], uv[2], uv[3]}, card.at("messages"), card.at("filter_mask")});
        visibleCards_.push_back(int(appealCards_.size()) - 1);
    }
    for (const auto& item : customization.at("bases"))
        boardBases_.push_back({item.at("id"), item.at("image"), item.at("thumbnail")});
    for (const auto& item : customization.at("stickers"))
        boardStickers_.push_back({item.at("id"), item.at("image"), item.at("thumbnail"), item.at("name"),
                                  item.at("width"), item.at("height")});
    const auto achievements = Json::parse(readAsset("subscreen/achievements.json"));
    achievementCategories_ = achievements.at("categories").get<std::vector<std::string>>();
    for (const auto& item : achievements.at("items")) achievements_.push_back({item.at("id"), item.at("category"),
        item.at("reward_type"), item.at("reward"), item.at("prerequisite"), item.at("goal"), item.at("parameter"),
        0, item.at("title"), item.at("metric")});
    // Prepare extracted crew/voice indexes on this same startup worker; the
    // first profile lookup must not enumerate their files on the render thread.
    crewFamilies();
    SDL_Log("SDVX catalogs ready: %zu songs, %.1f ms", songs_.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void Cabinet::prepareProfile() {
    loadProfile();
    rebuildFolders();
    activeFolder_ = 0;
    rebuildSongs();
    updateAchievements();
    stageOptions_ = options_;
}

void Cabinet::openDocument() {
    if (auto* ui = render_.ui()) {
        document_ = ui->LoadDocument("ui/cabinet.rml");
        if (!document_) throw std::runtime_error("Missing ui/cabinet.rml");
        document_->Show();
        inputDocument_ = ui->LoadDocument("ui/menu-input.rml");
        if (!inputDocument_) throw std::runtime_error("Missing ui/menu-input.rml");
        inputDocument_->Show();
    }
}

void Cabinet::closeDocuments() {
    if (inputDocument_) inputDocument_->Close();
    if (document_) document_->Close();
    inputDocument_ = document_ = nullptr;
    inputView_ = gaugeDrag_ = nullptr;
    pendingFields_.clear();
    visibleFields_.clear();
    uiLayers_.clear();
}

void Cabinet::preload() {
    if (startup_ && startup_->started < 0) {
        startup_->started = applicationTime();
        startup_->catalogs = std::async(std::launch::async, &Cabinet::loadCatalogs, this);
        audio_.prepareSample(KonamiSound, 1);
        // Compile the shared Cubism programs once the static warning has been presented.
        CrewModel::prepareShaders();
    }
    if (menuWarmup_ == MenuWarmup::Queued) return;
    if (menuWarmup_ == MenuWarmup::Pending) {
        if (startup_) {
            render_.queueImage(StartupKonami);
            render_.queueImage(StartupAmusement);
        }
        static constexpr std::string_view folders[]{
            "title_original", "shutter_nabla", "entry_original", "entry_wait", "entry_profile", "entry_checkin",
            "mode_flow", "room_flow", "caution_original", "cabinet_timer", "menu", "selection_ui", "song_row",
            "crew_selector", "crew_selector/background", "subscreen", "settings",
            "track_intro", "next_area", "matching", "stage_ready", "ex_track",
            "result_original", "result_score", "result_gauge", "result_profile"
        };
        render_.queueAfpData(folders);
        menuWarmup_ = MenuWarmup::Parsing;
        return;
    }
    if (!render_.afpDataReady()) return;
    menuWarmup_ = MenuWarmup::Queued;
    if (startup_) render_.queueAfp("title_original", "title", "in");
    render_.queueAfp("shutter_nabla", "shutter_default", "in");
    render_.queueAfp("shutter_nabla", "shutter_default", "out");
    render_.queueAfp("entry_original", "entry_07", "in");
    render_.queueAfp("entry_wait", "win_connect", "in");
    render_.queueAfp("entry_profile", "entry_02_id", "in");
    render_.queueAfp("entry_checkin", "win_paseli_gray", "in");
    render_.queueAfp("entry_checkin", "win_paseli_green", "in");
    render_.queueAfp("entry_checkin", "win_paseli_yellow", "in");
    render_.queueAfp("mode_flow", "mode_select", "in");
    render_.queueAfp("mode_flow", "mode_current", "in");
    render_.queueAfp("room_flow", "room_1", "in");
    render_.queueAfp("room_flow", "cursor", "in");
    render_.queueAfp("room_flow", "cursor", "decide");
    render_.queueAfp("room_flow", "cursor", "out");
    render_.queueAfp("caution_original", "caution", "0");
    render_.queueAfp("menu", "bg_room", "60");
    render_.queueAfp("menu", "ms_sel", "in_end");
    render_.queueAfp("menu", "cursor", "60");
    render_.queueAfp("menu", "operation_ms_sel", "operation_01");
    render_.queueAfp("song_row", "panel_music", "60");
    render_.queueAfp("selection_ui", "panel_folder", "in_end");
    render_.queueAfp("selection_ui", "panel_folder_sub", "in_end");
    render_.queueAfp("selection_ui", "window_select", "in");
    render_.queueAfp("selection_ui", "sort", "in");
    for (const auto path : {"selection_ui/panel_effective.png", "selection_ui/panel_excessive.png",
                            "selection_ui/panel_excessive_ars.png", "selection_ui/panel_ultra.png",
                            "selection_ui/panel_ultra_ars.png"})
        render_.queueImage(path);
    for (const auto label : {"adv", "exh", "grv", "hvn", "inf", "mxm", "nbl", "nov", "ult", "vvd", "xcd"})
        render_.queueImage(std::string("song_row/level_sel_") + label + ".png");
    for (int digit = 0; digit < 10; ++digit) {
        render_.queueImage("next_area/num_level_" + std::to_string(digit) + ".png");
        render_.queueImage("cabinet_timer/num_time_" + std::to_string(digit) + ".png");
    }
    render_.queueAfp("next_area", "loading", "loading_in");
    render_.queueAfp("next_area", "loading", "loading_loop");
    render_.queueAfp("next_area", "loading", "loading_out");
    render_.queueAfp("matching", "ready", "matching_in");
    render_.queueAfp("matching", "maching_ue", "mat_in");
    render_.queueAfp("matching", "maching_ue", "mat_loop");
    render_.queueAfp("matching", "hispeed", "hispeed_in");
    render_.queueImage("menu/art/bg_iseki.png");
    for (const auto path : {ShutterIn, ShutterOut, TrackIn, TrackOut, DifficultyOpen, DifficultyClose,
            EntryOpenLow, EntryCloseLow,
            "flow/se_08_big03_musicselect_nextarea.wav", "flow/se_01_short002_matching_ok.wav",
            "flow/se_08_big02_matching_ready2.wav", "stage_ready/ready.wav"})
        audio_.prepareSample(path);
}

Cabinet::~Cabinet() {
    closeDocuments();
}

void Cabinet::releaseGraphics() {
    closeDocuments();
    resultRecordArtReady_ = false;
    controllerTabLabelReady_ = false;
    navigator_.releaseGraphics();
}

void Cabinet::restoreGraphics() {
    menuWarmup_ = MenuWarmup::Pending;
    navigator_.restoreGraphics();
    if (!startup_ || startup_->profileReady) openDocument();
    profileCacheState_.fill(-2);
    entryProfileReady_ = false;
    modeLabelsReady_ = false;
    nextAreaLabelsReady_ = false;
    matchingCardReady_ = false;
}

int Cabinet::mappedButton(int key) const {
    if (key == SDL_SCANCODE_UNKNOWN) return -1;
    const auto it = std::find(keyMap_.begin(), keyMap_.end(), key);
    return it == keyMap_.end() ? -1 : static_cast<int>(it - keyMap_.begin());
}

int Cabinet::mappedControllerButton(int key) const {
    if (key <= 0) return -1;
    const auto it = std::find(controllerMap_.begin(), controllerMap_.end(), key);
    return it == controllerMap_.end() ? -1 : static_cast<int>(it - controllerMap_.begin());
}

float Cabinet::rotaryDelta(int side, float dx, float dy) const {
    if (controllerMode_ == ControllerMode::Joystick) {
        const bool useY = side == 0 ? controllerLeftUsesY_ : !controllerLeftUsesY_;
        return (useY ? dy : dx) * (controllerReverse_[side] ? -1.f : 1.f);
    }
    const bool useX = side == 0 ? leftUsesX_ : !leftUsesX_;
    return (useX ? dx : dy) * (reverse_[side] ? -1.f : 1.f);
}

void Cabinet::refreshBlasterPass() {
    blasterPassUntil_ = blasterPassLimitDate_;
    // Asphyxia's local enabled flag gives a 36-hour limit on each profile load.
    if (blasterPassActive_ && blasterPassUntil_ == 0)
        blasterPassUntil_ = unixTimeMs() + 36LL * 60 * 60 * 1000;
}

bool Cabinet::blasterPassActive() const {
    return blasterPassActive_ && unixTimeMs() < blasterPassUntil_;
}

void Cabinet::loadProfile() {
    const auto profile = Json::parse(readSave("cabinet.json"), nullptr, false);
    if (!profile.is_object()) return;
    favoriteSongs_ = profile.value("favorite_songs", std::vector<int>{});
    playerName_ = profile.value("player_name", std::string("LOCAL PLAYER"));
    entryNeedsName_ = profile.value("entry_needs_name", false);
    const auto crewId = profile.value("crew_id", crewInfo(0).id);
    const auto crewStyle = profile.value("crew_style", 0);
    for (const auto& family : crewFamilies()) {
        if (family.id != crewId) continue;
        const auto match = std::ranges::find_if(family.styles, [crewStyle](int index) {
            const auto& crew = crewInfo(index);
            return crew.style == crewStyle && crew.installed;
        });
        if (match != family.styles.end()) selectedCrew_ = *match;
        break;
    }
    crewOnMain_ = selectedCrew_;
    options_.crew = selectedCrew_;
    const auto cardId = profile.value("appeal_card", appealCards_[appealCard_].id);
    const auto card = std::find_if(appealCards_.begin(), appealCards_.end(), [&](const auto& item) { return item.id == cardId; });
    if (card != appealCards_.end()) appealCard_ = int(card - appealCards_.begin());
    boardStyle_ = std::clamp(profile.value("board_style", 0), 0, 1);
    const auto baseId = profile.value("board_base", boardBases_.front().id);
    const auto base = std::find_if(boardBases_.begin(), boardBases_.end(), [&](const auto& item) { return item.id == baseId; });
    boardBase_ = base == boardBases_.end() ? 0 : int(base - boardBases_.begin());
    auto loadSticker = [&](int id, std::optional<std::array<int, 2>> position) {
        const auto sticker = std::find_if(boardStickers_.begin(), boardStickers_.end(),
                                          [&](const auto& item) { return item.id == id; });
        if (sticker == boardStickers_.end()) return;
        const auto [column, row] = position.value_or(std::array{
            (StickerColumns - sticker->columns) / 2, (StickerRows - sticker->rows) / 2});
        const PlacedSticker placement{int(sticker - boardStickers_.begin()), column, row};
        if (stickerFits(placement, placedStickers_)) placedStickers_.push_back(placement);
    };
    if (const auto stickers = profile.find("board_stickers"); stickers != profile.end()) {
        for (const auto& sticker : *stickers)
            loadSticker(sticker.at("id"), std::array<int, 2>{sticker.at("x"), sticker.at("y")});
    } else loadSticker(profile.value("board_sticker", -1), std::nullopt);
    favoriteCards_ = profile.value("favorite_cards", std::vector<int>{});
    claimedAchievements_ = profile.value("achievements", std::vector<int>{});
    earnedTitles_ = profile.value("earned_titles", std::vector<int>{});
    generatedParts_ = profile.value("generated_parts", std::vector<int>{});
    packets_ = profile.value("packets", 0);
    blocks_ = profile.value("blocks", 0);
    paseli_ = std::max(0, profile.value("paseli", DefaultPaseli));
    blasterPassActive_ = profile.value("blaster_pass_active", true);
    blasterPassLimitDate_ = profile.value("blaster_pass_limit_date", int64_t{0});
    refreshBlasterPass();
    blasterEnergy_ = std::clamp(profile.value("blaster_energy", 0), 0, MaxBlasterEnergy);
    exChallengeUnlocks_ = profile.value("ex_challenge_unlocks", decltype(exChallengeUnlocks_){});
    for (auto& unlocked : exChallengeUnlocks_) unlocked = std::clamp(unlocked, 0, 4);
    skillLevel_ = profile.value("skill_level", 0);
    skillTitle_ = profile.value("skill_title", 0);
    if (skillTitle_ < 0 || skillTitle_ > 14) skillTitle_ = 0;
    clearedSkillCourses_ = profile.value("cleared_skill_courses", decltype(clearedSkillCourses_){});
    const auto stats = profile.value("play_stats", Json::object());
    playCount_ = stats.value("plays", 0);
    playDays_ = stats.value("days", 0);
    todayPlays_ = stats.value("today", 0);
    dayChain_ = stats.value("day_chain", 0);
    weekChain_ = stats.value("week_chain", 0);
    lastPlayDay_ = stats.value("last_day", int64_t(-1));
    lastPlayWeek_ = stats.value("last_week", int64_t(-1));
    const auto keys = profile.value("keys", std::vector<int>{});
    std::copy_n(keys.begin(), std::min(keys.size(), keyMap_.size()), keyMap_.begin());
    // An older profile could already assign F1 to a play button.
    if (keys.size() == Subscreen && std::find(keys.begin(), keys.end(), SDL_SCANCODE_F1) != keys.end())
        keyMap_[Subscreen] = SDL_SCANCODE_UNKNOWN;
    leftUsesX_ = profile.value("leftUsesX", false);
    reverse_ = profile.value("reverse", reverse_);
    const auto controllerKeys = profile.value("controller_keys", std::vector<int>{});
    std::copy_n(controllerKeys.begin(), std::min(controllerKeys.size(), controllerMap_.size()), controllerMap_.begin());
    controllerLeftUsesY_ = profile.value("controller_left_uses_y", false);
    controllerReverse_ = profile.value("controller_reverse", controllerReverse_);
    volume_ = std::clamp(profile.value("volume", 100) / (profile.value("options_schema", 1) < 4 ? 10 : 1), 0, 10);
    audio_.setGain(volume_ / 10.f);
    const auto nemsys = profile.value("nemsys", DefaultNemsysSkin);
    options_.nemsys = nemsys >= 0 && nemsys < NemsysSkinCount ? nemsys : DefaultNemsysSkin;
    const auto options = profile.value("options", std::vector<int>{});
    for (size_t i = 0; i < std::min<size_t>(options.size(), static_cast<size_t>(Option::AutoPlay)); ++i) {
        auto option = static_cast<Option>(i);
        int value = options[i];
        if (option == Option::Judgment && profile.value("options_schema", 1) == 1)
            value = static_cast<int>(std::lround(value / 4.));
        if (option == Option::Mirror && profile.value("options_schema", 1) < 3) value = value ? 2 : 0;
        if (option == Option::Brightness) value = std::clamp((value + 12) / 25 * 25, 50, 100);
        adjust(option, (value - optionValue(option)) / (option == Option::Brightness ? 25 : 1));
    }
    options_.fineTiming = profile.value("fine_timing", options_.fineTiming);
    options_.setTimingValue(0, profile.value("judgment_timing", options_.timingValue(0)));
    options_.setTimingValue(1, profile.value("display_timing", options_.timingValue(1)));
    const auto records = profile.value("records", Json::object());
    forceFolderCharts_.clear();
    records_.clear();
    for (const auto& [key, value] : records.items())
        records_.emplace(key, Record{value.value("score", 0), value.value("grade", ""), value.value("outcome", ""),
            value.value("force", 0), value.value("radar", std::array<int, 6>{})});
    if (!records_.empty()) {
        for (const auto& song : songs_) for (const auto& chart : song.charts) {
            const auto record = records_.find(chart.recordKey);
            if (record != records_.end() && !records.at(chart.recordKey).contains("radar"))
                record->second.radar = playerRadar(chart, record->second.score, 0, 0);
        }
    }
    songFoldersDirty_ = true;
    // Old profiles lack per-play FORCE. Keep their independent best score/lamp
    // intact; do not combine them into an achievement that was never played.
}

void Cabinet::saveProfile() const {
    Json profile = {{"keys", keyMap_}, {"leftUsesX", leftUsesX_}, {"reverse", reverse_}};
    profile["controller_keys"] = controllerMap_;
    profile["controller_left_uses_y"] = controllerLeftUsesY_;
    profile["controller_reverse"] = controllerReverse_;
    profile["crew_id"] = crewInfo(selectedCrew_).id;
    profile["crew_style"] = crewInfo(selectedCrew_).style;
    profile["favorite_songs"] = favoriteSongs_;
    profile["player_name"] = playerName_;
    profile["entry_needs_name"] = entryNeedsName_;
    profile["options_schema"] = 4;
    profile["fine_timing"] = options_.fineTiming;
    profile["judgment_timing"] = options_.timingValue(0);
    profile["display_timing"] = options_.timingValue(1);
    profile["volume"] = volume_;
    profile["nemsys"] = options_.nemsys;
    profile["appeal_card"] = appealCards_[appealCard_].id;
    profile["board_style"] = boardStyle_;
    profile["board_base"] = boardBases_[boardBase_].id;
    auto& stickers = profile["board_stickers"] = Json::array();
    for (const auto& sticker : placedStickers_)
        stickers.push_back({{"id", boardStickers_[sticker.index].id}, {"x", sticker.column}, {"y", sticker.row}});
    profile["favorite_cards"] = favoriteCards_;
    profile["achievements"] = claimedAchievements_;
    profile["earned_titles"] = earnedTitles_;
    profile["generated_parts"] = generatedParts_;
    profile["packets"] = packets_;
    profile["blocks"] = blocks_;
    profile["paseli"] = paseli_;
    profile["blaster_pass_active"] = blasterPassActive_;
    if (blasterPassLimitDate_ > 0) profile["blaster_pass_limit_date"] = blasterPassLimitDate_;
    profile["blaster_energy"] = blasterEnergy_;
    profile["ex_challenge_unlocks"] = exChallengeUnlocks_;
    profile["skill_level"] = skillLevel_;
    profile["skill_title"] = skillTitle_;
    profile["cleared_skill_courses"] = clearedSkillCourses_;
    profile["play_stats"] = {{"plays",playCount_},{"days",playDays_},{"today",todayPlays_},
        {"day_chain",dayChain_},{"week_chain",weekChain_},{"last_day",lastPlayDay_},{"last_week",lastPlayWeek_}};
    if (!cardId_.empty()) profile["card_id"] = cardId_;
    auto& options = profile["options"] = Json::array();
    // AUTO is a preview toggle for this launch.
    for (int i = 0; i < static_cast<int>(Option::AutoPlay); ++i) {
        const auto option = static_cast<Option>(i);
        if (option == Option::Judgment) options.push_back(static_cast<int>(options_.judgmentOffset / 4));
        else if (option == Option::Display) options.push_back(static_cast<int>(options_.displayOffset / 4));
        else options.push_back(optionValue(option));
    }
    auto& records = profile["records"] = Json::object();
    for (const auto& [key, value] : records_)
        records[key] = {{"score", value.score}, {"grade", value.grade}, {"outcome", value.outcome},
            {"force", value.force}, {"radar", value.radar}};
    writeSave("cabinet.json", profile.dump());
}

const Cabinet::Chart& Cabinet::chart() const {
    if (activeCourse_) {
        const auto [songIndex, chartIndex] = courses_[*activeCourse_].tracks[track_ - 1];
        return songs_[songIndex].charts[chartIndex];
    }
    return song().charts[chartIndex_];
}

bool Cabinet::skillPassed() const {
    return activeCourse_ && sessionResults_.size() == courses_[*activeCourse_].tracks.size()
        && lower(sessionResults_.back().result.outcome) != "crash";
}

int Cabinet::skillAchievement() const {
    const auto trackCount = static_cast<int>(courses_[*activeCourse_].tracks.size());
    // 1805f59e0 awards 100–200% on completion and uses course progress on failure.
    if (skillPassed()) {
        int score = 0;
        for (const auto& track : sessionResults_) score += track.result.score;
        return 100 + std::clamp((score / trackCount - 9000000) / 10000, 0, 100);
    }
    int cleared = 0;
    for (const auto& track : sessionResults_) cleared += lower(track.result.outcome) != "crash";
    return std::min(99, static_cast<int>((cleared + result_.gauge) * 100 / trackCount));
}

std::string_view Cabinet::skillResultIntro() const {
    return skillPassed() ? "in_success" : "in_fail";
}

double Cabinet::skillResultIntroDuration() const {
    return activeCourse_ ? render_.afpDuration("skill_effects", skillResultIntro(), "in") : 0;
}

int Cabinet::skillBonusRemaining(int amount) const {
    if (amount <= 0) return 0;
    // 1802fbdb0/1802fb6c0: integer increments with a 60-update target.
    const int interval = std::max(1, 60 / amount);
    const int step = std::max(1, amount / 60);
    const int updates = static_cast<int>((now_ - skillBonusAt_) * .12);
    return std::max(0, amount - (updates / interval) * step);
}

void Cabinet::finishSummaryCount() {
    summaryCountDone_ = true;
    if (summaryCountPlaying_) {
        summaryCountPlaying_ = false;
        audio_.stopEffectLoop();
        play(CountEnd, twoDxGain(43));
    }
    if (!activeCourse_) play("result_force/audio/total_info.wav");
}

const Cabinet::Record& Cabinet::record(const Selection& track) const {
    static const Record unplayed;
    const auto it = records_.find(keyFor(track));
    return it == records_.end() ? unplayed : it->second;
}

std::vector<std::pair<std::string_view, int>> Cabinet::topForceCharts() const {
    std::vector<std::pair<std::string_view, int>> charts;
    charts.reserve(records_.size());
    for (const auto& [key, record] : records_)
        if (record.force > 0) charts.emplace_back(key, record.force);
    const auto count = std::min<size_t>(50, charts.size());
    std::partial_sort(charts.begin(), charts.begin() + count, charts.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    charts.resize(count);
    return charts;
}

int Cabinet::totalForce() const {
    int total = 0;
    for (const auto& [key, force] : topForceCharts()) total += force;
    return total;
}

std::array<int, 6> Cabinet::playerRadar(const Chart& chart, int score, int exScore, int maxExScore) {
    // 180238cc0 computes each axis independently from the XML radar, score and EX score.
    constexpr std::array excludedSongs{840, 1219, 1259, 1438, 1751};
    std::array<int, 6> result{};
    if (std::ranges::find(excludedSongs, chart.track.id) != excludedSongs.end()) return result;
    const int level = chart.track.level / 10;
    const int levelMultiplier = std::min(2 * (level + 30), 100);
    const int exWeight = maxExScore > 0 && exScore <= maxExScore
        ? std::max(0, 1000 - (2 << ((21 - level) / 2)) * (maxExScore - exScore)) : 0;
    for (int axis = 0; axis < int(result.size()); ++axis) {
        const int base = std::clamp(chart.radar[axis], 0, 200) * 100;
        const int normal = int(int64_t(base) * score * 3 / 40000000);
        const int ex = base * exWeight / 4000;
        result[axis] = std::min(base, normal + ex) * levelMultiplier;
    }
    return result;
}

std::pair<int, int> Cabinet::chartRange(const Song& entry, bool challenge) const {
    const int count = int(entry.charts.size());
    if (entry.challenge < 0) return {0, challenge ? 0 : count};
    const int unlocked = std::min(count, exChallengeUnlocks_[entry.challenge]);
    if (!challenge) return {0, unlocked};
    if (entry.challenge == 0) return {unlocked, count};
    const int available = std::min(count, *std::min_element(exChallengeUnlocks_.begin(),
        exChallengeUnlocks_.begin() + entry.challenge));
    return {unlocked, std::max(unlocked, available)};
}

bool Cabinet::folderChartRows(FolderKind kind) {
    using enum FolderKind;
    switch (kind) {
    case Level: case LevelAll: case LevelExact:
    case Grades: case Grade: case Lamps: case Lamp: case NearGrades: case NearGrade:
    case SkillLevels: case SkillLevel: case Volforce: case ForcePotential:
    case PlayerRadar: case PlayerAxis: case Played: case EffectAxis:
        return true;
    default: return false;
    }
}

std::vector<std::pair<int, int>> Cabinet::folderRows(const SongFolder& folder) const {
    using enum FolderKind;
    std::vector<std::pair<int, int>> rows;
    const bool fixed = folderChartRows(folder.kind);
    if (folder.kind == SkillLevels || folder.kind == SkillLevel) {
        for (const auto& course : courses_)
            if (course.level > 0 && (folder.kind == SkillLevels || course.level == folder.value))
                for (const auto& [song, chart] : course.tracks) {
                    const auto [begin, end] = chartRange(songs_[song], false);
                    if (chart >= begin && chart < end) rows.emplace_back(song, chart);
                }
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        return rows;
    }

    constexpr std::array nearTargets{10000000, 9900000, 9800000, 9700000};
    const Result potential{.outcome = "puc", .grade = "s", .score = 10000000};
    for (int s = 0; s < int(songs_.size()); ++s) {
        const auto& song = songs_[s];
        const auto [begin, end] = chartRange(song, folder.kind == ExChallenge);
        if (begin == end) continue;
        bool included = true;
        switch (folder.kind) {
        case Kana: included = song.kana == folder.value; break;
        case Genre: included = folder.value ? (song.genre & folder.value) != 0 : song.genre == 0; break;
        case Contests: included = !song.contests.empty(); break;
        case Contest: included = std::ranges::find(song.contests, folder.value) != song.contests.end(); break;
        case Version: included = song.version == folder.value; break;
        case Date: included = song.date / 10000 == folder.value; break;
        case Favorites: included = std::ranges::find(favoriteSongs_, song.charts.front().track.id) != favoriteSongs_.end(); break;
        case Beginner: included = song.beginner; break;
        default: break;
        }
        if (!included) continue;
        if (!fixed) {
            rows.emplace_back(s, -1);
            continue;
        }
        for (int c = begin; c < end; ++c) {
            const auto& chart = song.charts[c];
            if (folder.kind == Level || folder.kind == LevelAll || folder.kind == LevelExact) {
                const int level = folder.kind == LevelExact ? chart.track.level : chart.track.level / 10;
                if (level == folder.value) rows.emplace_back(s, c);
                continue;
            }
            if (folder.kind == EffectAxis) {
                // 1803351a0 sets a bit for every axis tied for this chart's largest value.
                if (chart.radar[folder.value] == *std::ranges::max_element(chart.radar)) rows.emplace_back(s, c);
                continue;
            }
            const auto found = records_.find(chart.recordKey);
            const auto* saved = found == records_.end() ? nullptr : &found->second;
            const int grade = saved ? resultGrade(saved->grade) : 0;
            const int lamp = saved ? lampRank(saved->outcome) : 0;
            const int score = saved ? saved->score : 0;
            const auto nearGrade = [&](int target) {
                if (!saved) return false;
                if (target == 0) return (lamp == 5 && score >= 9985000) || (lamp < 5 && score >= 9995000);
                const int difference = nearTargets[target] - score;
                return difference > 0 && difference < 10000;
            };
            switch (folder.kind) {
            case Grade: included = (grade <= 10 ? grade : 0) == folder.value; break;
            case Lamp: included = (lamp == 7 ? 6 : lamp) == folder.value; break;
            case NearGrade: included = nearGrade(folder.value); break;
            case NearGrades: included = nearGrade(0) || nearGrade(1) || nearGrade(2) || nearGrade(3); break;
            case Volforce: included = saved && saved->force > 0 &&
                std::ranges::find(forceFolderCharts_, chart.recordKey, &std::pair<std::string_view, int>::first) != forceFolderCharts_.end(); break;
            case ForcePotential: included = forceFolderCharts_.size() == 50 &&
                chartForce(chart.track.level, potential) > std::max(forceFolderCharts_.back().second, saved ? saved->force : 0); break;
            case PlayerRadar:
                included = false;
                if (saved) for (int axis = 0; axis < int(playerRadarCutoffs_.size()); ++axis)
                    included |= saved->radar[axis] > 0 && saved->radar[axis] >= playerRadarCutoffs_[axis];
                break;
            case PlayerAxis:
                included = saved && saved->radar[folder.value] > 0 && saved->radar[folder.value] >= playerRadarCutoffs_[folder.value];
                break;
            case Played: included = saved != nullptr; break;
            default: included = true; break;
            }
            if (included) rows.emplace_back(s, c);
        }
    }
    return rows;
}

int Cabinet::folderIndex(FolderKind kind, int value) const {
    const auto found = std::ranges::find_if(songFolders_, [&](const auto& folder) {
        return folder.kind == kind && folder.value == value;
    });
    return found == songFolders_.end() ? -1 : int(found - songFolders_.begin());
}

void Cabinet::rebuildFolders(FolderKind group, int groupValue) {
    using enum FolderKind;
    const auto focused = songFolders_.empty() ? SongFolder{All, 0, "ALL"} : songFolders_[wheelIndex_];
    const auto active = activeFolder_ < 0 ? SongFolder{All, -1, ""} : songFolders_[activeFolder_];
    folderGroup_ = group;
    folderGroupValue_ = groupValue;
    songFoldersDirty_ = false;
    songScrollIndex_ = -1.f;
    forceFolderCharts_ = topForceCharts();
    playerRadarCutoffs_.fill(0);
    if (!records_.empty()) {
        // 1802390e0 selects the best difficulty of each song, then the top 50 per axis.
        std::array<std::vector<int>, 6> axes;
        for (const auto& song : songs_) {
            const auto [begin, end] = chartRange(song, false);
            std::array<int, 6> best{};
            for (int c = begin; c < end; ++c) {
                const auto record = records_.find(song.charts[c].recordKey);
                if (record != records_.end())
                    for (int axis = 0; axis < int(best.size()); ++axis)
                        best[axis] = std::max(best[axis], record->second.radar[axis]);
            }
            for (int axis = 0; axis < int(best.size()); ++axis)
                if (best[axis] > 0) axes[axis].push_back(best[axis]);
        }
        for (int axis = 0; axis < int(axes.size()); ++axis) {
            auto& values = axes[axis];
            if (values.size() >= 50) {
                std::nth_element(values.begin(), values.begin() + 49, values.end(), std::greater<int>{});
                playerRadarCutoffs_[axis] = values[49];
            }
        }
    }
    songFolders_.clear();

    constexpr std::array kana{"あ行","か行","さ行","た行","な行","は行","ま行","や行","ら行","わ行","英数字・その他"};
    constexpr std::array grades{"NO PLAY","D","C","B","A","A+","AA","AA+","AAA","AAA+","S"};
    constexpr std::array lamps{"NO MEDAL","PLAYED","COMPLETE","EXCESSIVE COMPLETE","MAXXIVE COMPLETE","UC","PUC"};
    constexpr std::array near{"ALMOST PUC","ALMOST S","ALMOST AAA+","ALMOST AAA"};
    constexpr std::array axes{"NOTES","PEAK","TSUMAMI","TRICKY","HAND-TRIP","ONE-HAND"};
    constexpr std::array versions{"","SOUND VOLTEX BOOTH","SOUND VOLTEX II -infinite infection-","SOUND VOLTEX III GRAVITY WARS",
        "SOUND VOLTEX IV HEAVENLY HAVEN","SOUND VOLTEX VIVID WAVE","SOUND VOLTEX EXCEED GEAR","SOUND VOLTEX ∇"};
    constexpr std::array skillNames{"","岳翔","流星","月衝","瞬光","天極","烈風","雷電","麗華","魔騎士","剛力羅","或帝滅斗","暴龍天"};

    const auto addChild = [&](FolderKind kind, int value, std::string name) {
        songFolders_.push_back({kind, value, std::move(name), 0, {}, true});
    };
    const auto addMain = [&](FolderKind kind, int value, std::string name, bool expandable = false) {
        const bool expanded = expandable && group == kind && groupValue == value;
        songFolders_.push_back({kind, value, std::move(name), 0, {}, false, expandable});
        if (!expanded) return;
        switch (kind) {
        case Level: {
            const auto prefix = "LEVEL " + std::to_string(value);
            addChild(LevelAll, value, prefix + " ALL");
            for (int tenth = 0; tenth < 10; tenth += value == 17 ? 5 : 1) {
                auto name = prefix;
                if (value != 17 || tenth != 0) name += "." + std::to_string(tenth);
                addChild(LevelExact, value * 10 + tenth, std::move(name));
            }
            break;
        }
        case Grades:
            for (int grade = 10; grade >= 0; --grade) addChild(Grade, grade, grades[grade]);
            break;
        case NearGrades:
            for (int i = 0; i < int(near.size()); ++i) addChild(NearGrade, i, near[i]);
            break;
        case Lamps:
            for (int lamp = 6; lamp >= 0; --lamp) addChild(Lamp, lamp, lamps[lamp]);
            break;
        case Genres:
            for (const auto& [bit, title] : std::initializer_list<std::pair<int, const char*>>{
                {1,"EXIT TUNES"},{2,"FLOOR"},{16,"BEMANI"},{4,"東方"},{8,"ボカロ"},
                {32,"SDVXオリジナル"},{64,"POPS＆アニメ"},{128,"ひなビタ♪/バンめし♪"},{0,"その他"}})
                addChild(Genre, bit, title);
            break;
        case Contests:
            for (int i = 0; i < int(floorContests_.size()); ++i) addChild(Contest, i + 1, floorContests_[i]);
            break;
        case Versions:
            for (int v : versions_)
                if (v > 0 && v < int(versions.size())) addChild(Version, v, versions[v]);
            break;
        case Dates: {
            std::vector<int> years;
            for (const auto& song : songs_) years.push_back(song.date / 10000);
            std::ranges::sort(years);
            years.erase(std::unique(years.begin(), years.end()), years.end());
            for (int year : years) addChild(Date, year, std::to_string(year) + "年");
            break;
        }
        case SkillLevels:
            for (int i = 1; i < int(skillNames.size()); ++i) {
                const auto number = i == 12 ? std::string("∞") : (i < 10 ? "0" : "") + std::to_string(i);
                addChild(SkillLevel, i, "SKILL LEVEL " + number + " " + skillNames[i]);
            }
            break;
        case EffectRadar: case PlayerRadar:
            if (kind == PlayerRadar) addChild(Played, 0, "PLAYED");
            for (int i = 0; i < int(axes.size()); ++i)
                addChild(kind == EffectRadar ? EffectAxis : PlayerAxis, i, axes[i]);
            break;
        default: break;
        }
    };

    addMain(All, 0, "ALL");
    for (int i = 0; i < int(kana.size()); ++i) addMain(Kana, i, kana[i]);
    for (int level = 1; level <= 20; ++level) addMain(Level, level, "LEVEL " + std::to_string(level), level >= 17);
    addMain(Grades, 0, "スコアグレード", true);
    addMain(NearGrades, 0, "ALMOST", true);
    addMain(Lamps, 0, "クリアマーク", true);
    addMain(Genres, 0, "ジャンルから探す", true);
    addMain(Contests, 0, "コンテストから探す", true);
    addMain(Versions, 0, "バージョンから探す", true);
    addMain(Dates, 0, "配信日", true);
    addMain(SkillLevels, 0, "SKILL LEVEL", true);
    addMain(Favorites, 0, "お気に入り");
    if (!forceFolderCharts_.empty()) addMain(Volforce, 0, "VOLFORCE");
    if (forceFolderCharts_.size() == 50) addMain(ForcePotential, 0, "VOLFORCE POTENTIAL");
    addMain(EffectRadar, 0, "EFFECT RADAR", true);
    if (!records_.empty()) addMain(PlayerRadar, 0, "PLAYER RADAR", true);
    const SongFolder challenge{ExChallenge, 0, "EXTRACK CHALLENGE"};
    if (exTrack() && !folderRows(challenge).empty()) songFolders_.push_back(challenge);
    addMain(Beginner, 0, "初めての方へオススメ");

    for (auto& folder : songFolders_) {
        const auto rows = folderRows(folder);
        folder.count = int(rows.size());
        if (rows.empty() || records_.empty()) continue;

        // 18028b8e0 takes the minimum lamp and grade over the folder's charts.
        const int variants = folderChartRows(folder.kind) ? 1 : int(folder.badges.size());
        for (int difficulty = 0; difficulty < variants; ++difficulty) {
            int lamp = 7, grade = 10;
            for (const auto& [s, c] : rows) {
                const auto& song = songs_[s];
                const auto [begin, end] = chartRange(song, folder.kind == ExChallenge);
                const auto& chart = song.charts[c < 0 ? std::clamp(difficulty, begin, end - 1) : c];
                const auto saved = records_.find(chart.recordKey);
                if (saved == records_.end()) {
                    lamp = grade = 0;
                    break;
                }
                lamp = std::min(lamp, lampRank(saved->second.outcome));
                const int rank = resultGrade(saved->second.grade);
                grade = std::min(grade, rank <= 10 ? rank : 0);
            }
            folder.badges[difficulty] = {lamp, grade};
        }
        if (variants == 1) folder.badges.fill(folder.badges.front());
    }
    activeFolder_ = folderIndex(active.kind, active.value);
    const int focus = folderIndex(focused.kind, focused.value);
    const int parent = folderIndex(group, groupValue);
    wheelIndex_ = focus >= 0 ? focus : std::max(0, parent);
}

int Cabinet::displayChart(const Song& entry) const {
    const auto [begin, end] = chartRange(entry, exChallengeFolder());
    return std::clamp(exChallengeFolder() ? chartIndex_ : preferredChart_, begin, end - 1);
}

void Cabinet::rebuildSongs() {
    if (activeFolder_ < 0) return;
    const int selected = visibleSongs_.empty() ? -1 : visibleSongs_[songIndex_];
    const int selectedChart = chartIndex_;
    const auto& folder = songFolders_[activeFolder_];
    auto filtered = folderRows(folder);
    // Empty folders leave focus on the folder row. The settings preview keeps
    // its previous song until there is a new valid selection.
    if (filtered.empty()) {
        wheelIndex_ = activeFolder_;
        activeFolder_ = -1;
        wheel_ = Wheel::Folder;
        previewAt_ = -1;
        if (page_ == Page::Songs) audio_.stopPreview(true);
        return;
    }

    std::stable_sort(filtered.begin(), filtered.end(), [&](auto a, auto b) {
        const auto& x = songs_[a.first];
        const auto& y = songs_[b.first];
        const auto& xc = x.charts[a.second < 0 ? displayChart(x) : a.second];
        const auto& yc = y.charts[b.second < 0 ? displayChart(y) : b.second];
        using enum FolderKind;
        static const Record unplayed;
        const auto recordFor = [&](const Chart& chart) -> const Record& {
            const auto found = records_.find(chart.recordKey);
            return found == records_.end() ? unplayed : found->second;
        };
        if (sort_ == 0) {
            if (folder.kind == Volforce && recordFor(xc).force != recordFor(yc).force) return recordFor(xc).force > recordFor(yc).force;
            if (folder.kind == Played && recordFor(xc).score != recordFor(yc).score) return recordFor(xc).score > recordFor(yc).score;
            if (folder.kind == PlayerAxis || folder.kind == EffectAxis) {
                const int left = folder.kind == PlayerAxis ? recordFor(xc).radar[folder.value] : xc.radar[folder.value];
                const int right = folder.kind == PlayerAxis ? recordFor(yc).radar[folder.value] : yc.radar[folder.value];
                if (left != right) return left > right;
            }
        }
        if (sort_ == 2 && x.artistKey != y.artistKey) return x.artistKey < y.artistKey;
        if (sort_ == 3 && xc.track.level != yc.track.level) return xc.track.level < yc.track.level;
        if (sort_ == 4 && x.date != y.date) return x.date > y.date;
        if (sort_ == 5) {
            const int left = lampRank(recordFor(xc).outcome), right = lampRank(recordFor(yc).outcome);
            if (left != right) return left > right;
        }
        if (sort_ == 6) {
            const int left = recordFor(xc).score, right = recordFor(yc).score;
            if (left != right) return left > right;
        }
        return std::tie(x.titleKey, x.artistKey, x.date, xc.track.level, xc.track.id) <
               std::tie(y.titleKey, y.artistKey, y.date, yc.track.level, yc.track.id);
    });
    visibleSongs_.clear();
    visibleCharts_.clear();
    for (const auto& [song, chart] : filtered) {
        visibleSongs_.push_back(song);
        visibleCharts_.push_back(chart);
    }
    const auto exact = std::ranges::find(filtered, std::pair{selected, selectedChart});
    const auto sameSong = std::ranges::find(visibleSongs_, selected);
    songIndex_ = exact != filtered.end() ? int(exact - filtered.begin())
        : sameSong != visibleSongs_.end() ? int(sameSong - visibleSongs_.begin()) : 0;
    chartIndex_ = visibleCharts_[songIndex_] < 0 ? displayChart(song()) : visibleCharts_[songIndex_];
    if (page_ == Page::Songs && (visibleSongs_[songIndex_] != selected || chartIndex_ != selectedChart)) audio_.stopPreview();
    previewAt_ = wheel_ == Wheel::Songs ? now_ + SongPreviewDelay : -1;
}

std::pair<int, int> Cabinet::songListPosition() const {
    const int songs = activeFolder_ < 0 ? 0 : int(visibleSongs_.size());
    const int index = wheel_ == Wheel::Songs ? activeFolder_ + 1 + songIndex_
        : wheelIndex_ + (activeFolder_ >= 0 && wheelIndex_ > activeFolder_ ? songs : 0);
    return {index, int(songFolders_.size()) + songs};
}

std::string Cabinet::songScrollLabel() const {
    if (page_ != Page::Songs || (transition_ && transition_->next != Page::Songs)) return {};
    if (wheel_ == Wheel::Folder) return songFolders_[wheelIndex_].name;

    const auto& current = song();
    const auto& track = chart().track;
    if (track.id == 1438 || track.id == 1259) return {};
    using enum FolderKind;
    int sort = sort_;
    if (sort == 0) {
        // The original folder supplies the fallback formatter when SORT is default.
        switch (songFolders_[activeFolder_].kind) {
        case Level: case LevelAll: case LevelExact: sort = 3; break;
        case Version: case Date: case ExChallenge: case Beginner: sort = 4; break;
        case Lamps: case Lamp: sort = 5; break;
        case Grades: case Grade: case NearGrades: case NearGrade: case Played: sort = 6; break;
        default: sort = 1; break;
        }
    }
    switch (sort) {
    case 1: case 2: {
        const auto& key = sort == 1 ? current.titleKey : current.artistKey;
        const char* start = key.data();
        size_t length = key.size();
        auto code = length == 0 ? 0 : SDL_StepUTF8(&start, &length);
        constexpr std::u32string_view voiced = U"がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽゔ";
        constexpr std::u32string_view unvoiced = U"かきくけこさしすせそたちつてとはひふへほはひふへほう";
        constexpr std::u32string_view kana = U"あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんぁぃぅぇぉっゃゅょ";
        if (const auto index = voiced.find(code); index != voiced.npos) code = unvoiced[index];
        if (kana.find(code) == kana.npos) return "etc.";
        char first[5]{};
        SDL_UCS4ToUTF8(code + 0x60, first);
        return first;
    }
    case 3:
        return track.level < 170 ? "Lv." + std::to_string(track.level / 10)
            : "Lv" + std::to_string(track.level / 10) + "." + std::to_string(track.level % 10);
    case 4: {
        if (current.date == 0) return {};
        char date[16];
        std::snprintf(date, sizeof(date), "%02d/%02d/%02d", current.date / 10000 % 100, current.date / 100 % 100, current.date % 100);
        return date;
    }
    case 5: {
        constexpr std::array lamps{"NO MEDAL", "PLAYED", "COMPLETE", "COMPLETE", "COMPLETE", "UC", "PUC", "PUC"};
        return lamps[lampRank(record(track).outcome)];
    }
    case 6: {
        constexpr std::array grades{"D", "C", "B", "A", "A+", "AA", "AA+", "AAA", "AAA+", "S", "NO GRADE"};
        return grades[resultGrade(record(track).grade) - 1];
    }
    default: return {};
    }
}

void Cabinet::moveSongList(int steps) {
    const auto [position, total] = songListPosition();
    const int count=activeFolder_<0?0:int(visibleSongs_.size());
    const int target=wrap(position+steps,total);
    if (target == position) return;
    if (activeFolder_>=0 && target>activeFolder_ && target<=activeFolder_+count) {
        audio_.stopPreview();
        wheel_=Wheel::Songs; songIndex_=target-activeFolder_-1; chartIndex_=visibleCharts_[songIndex_]<0?displayChart(song()):visibleCharts_[songIndex_]; previewAt_=now_+SongPreviewDelay;
    } else {
        wheel_=Wheel::Folder; wheelIndex_=target-(activeFolder_>=0 && target>activeFolder_?count:0);
        audio_.stopPreview(true); previewAt_=-1;
    }
}

void Cabinet::startSongSelectionAudio() {
    const bool previewSelected = wheel_ == Wheel::Songs && chart().installed && chart().track.id != AutomationParadise2019;
    if (audio_.playMusic(SongSelectMusic, true, previewSelected ? 0.f : 1.f))
        activeMusic_ = SongSelectMusic;
    previewAt_ = previewSelected ? now_ + SongPreviewDelay : -1;
}

void Cabinet::resetInput() {
    held_.fill(false);
    knobRemainder_.fill(0);
    fxCombined_ = false;
    startUsed_ = false;
    resultRetryAt_ = -1;
    finger_.reset();
    gaugeDrag_ = nullptr;
    dragged_ = false;
    stickerDragging_ = false;
}

void Cabinet::play(std::string_view path, float gain) { audio_.sfx(path, gain); }

void Cabinet::notice(std::string text) {
    message_ = std::move(text);
    messageUntil_ = now_ + 2600;
}

void Cabinet::enter(Page next, bool newTrack) {
    const Page previous = page_;
    sortOpen_=songConfirm_=songConfirmAccept_=false; songOverlayOutAt_=-1;
    page_ = next;
    if (page_ == Page::Songs) songScrollIndex_ = songScrollFrame_ = -1.f;
    cardFilterOpen_ = false;
    messageUntil_ = 0;
    achievementPopupAt_ = -1;
    if (next != Page::Room && next != Page::Crew) {
        crewSwitchPending_ = false;
        startAfterSubscreen_ = false;
    }
    roomDepartureAt_ = -1;
    entered_ = now_;
    characterView_ = false;
    resetInput();
    audio_.stopPreview();
    audio_.stopEffectLoop();
    previewAt_ = -1;
    songPromptPending_ = page_ == Page::Songs && newTrack && track_ == 1 && sessionType_ != StartType::Premium;
    songPromptAt_ = -1;
    if (page_ == Page::Songs && newTrack && exTrack()) {
        activeFolder_ = 0;
        rebuildFolders();
        const auto challenge = std::find_if(songFolders_.begin(), songFolders_.end(),
            [](const SongFolder& folder) { return folder.kind == FolderKind::ExChallenge; });
        const bool hasChallenge = challenge != songFolders_.end();
        if (hasChallenge) activeFolder_ = int(challenge - songFolders_.begin());
        wheelIndex_ = activeFolder_;
        wheel_ = Wheel::Songs;
        rebuildSongs();
        if (hasChallenge) {
            // 180286200 focuses the first eligible song in campaign order.
            songIndex_ = int(std::min_element(visibleSongs_.begin(), visibleSongs_.end(), [&](int a, int b) {
                return songs_[a].challenge < songs_[b].challenge;
            }) - visibleSongs_.begin());
            chartIndex_ = displayChart(song());
        }
    } else if (page_ == Page::Songs && songFoldersDirty_) {
        rebuildFolders(folderGroup_, folderGroupValue_);
        rebuildSongs();
    }
    std::string music;
    switch (page_) {
    case Page::Title:
        practiceUntil_ = 0;
        continuing_ = false;
        titleCallPending_ = true;
        pin_.clear();
        pendingPin_.clear();
        music = "menu/audio/bgm_ver7_01_title.wav";
        break;
    case Page::Pin: case Page::Room: case Page::Generator: case Page::Valkyrie:
    case Page::Caution: case Page::Mode: case Page::Credit: case Page::Crew: case Page::Skill:
        music = "menu/audio/bgm_ver7_02_modeselect.wav"; break;
    case Page::Matching:
        music = "flow/matching.wav";
        matchingGo_ = matchingSound_ = false;
        matchingCardReady_ = false;
        break;
    case Page::Result: music = "result/bgm.wav"; break;
    case Page::Summary: music = "entry_flow/total.wav"; break;
    case Page::Exit: if (premiumExit()) music = activeMusic_; break;
    case Page::Settings: music = activeMusic_; break;
    default: break;
    }
    if (music != activeMusic_) {
        activeMusic_.clear();
        if (music.empty()) audio_.stopMusic();
        else if (audio_.playMusic(music, true)) activeMusic_ = std::move(music);
    }
    if (page_ == Page::Songs && !transition_) startSongSelectionAudio();
    if (page_ == Page::Result) {
        resultRecordArtReady_ = false;
        resultRewardsAt_ = -1;
    }
    if (page_ == Page::Credit) {
        creditFocus_ = 0;
        paymentAcceptedAt_ = paymentReceiptAt_ = paymentReceiptOutAt_ = -1;
        paymentReceiptPrice_ = 0;
        paymentCanceled_ = false;
    }
    using enum NavigatorCue;
    auto cue = Silent;
    switch (page_) {
    case Page::Pin: {
        const auto card = localCardState();
        cardId_ = card.substr(0, 16);
        pinPhase_ = card.size() == 17 && card.back() == '1' ? PinPhase::Verify : PinPhase::Create;
        pin_.clear();
        pendingPin_.clear();
        pinFocus_ = 0;
        pinVerification_.reset();
        pinError_ = cardId_.empty() ? "カード情報を保存できませんでした" : "";
        entryAuthenticated_ = false;
        if (pinPhase_ == PinPhase::Create) entryNeedsName_ = true;
        draftName_.clear();
        navigator_.play(Silent, selectedCrew_, now_);
        entryStep_ = cardId_.empty() ? EntryStep::Registration : EntryStep::CardRead;
        if (!transition_ || transition_->next != Page::Pin) entryStep(entryStep_);
        break;
    }
    case Page::Room:
        if (previous == Page::Credit) {
            roomStarted_ = now_;
            roomFocus_ = 1;
            cue = Room;
        } else if (previous != Page::Settings && previous != Page::Crew) {
            roomStarted_ = now_;
            cue = RoomReturn;
        }
        break;
    case Page::Generator: case Page::Valkyrie:
        generatorPhase_ = 0;
        generatorCount_ = 1;
        generatorAt_ = now_;
        cue = page_ == Page::Generator ? Generator : Valkyrie;
        break;
    case Page::Mode:
        if (previous != Page::Credit) modeStarted_ = now_;
        if (previous == Page::Pin) play("entry_flow/mode.wav");
        if (previous != Page::Credit && previous != Page::Settings) cue = Mode;
        break;
    case Page::Credit: play(EntryOpenLow); break;
    case Page::Caution:
        cautionOpened_ = cautionClosing_ = false;
        render_.queueAfp("shutter_nabla", "shutter_default", "loop");
        render_.queueAfp("track_intro", trackIntroName(), "in");
        render_.queueAfp("track_intro", trackIntroName(), "out");
        play("caution_original/audio/s207_caution.wav", twoDxGain(98));
        break;
    case Page::Skill:
        activeCourse_.reset();
        render_.queueAfp("next_area", "loading", "loading_in");
        play("skill_effects/audio/select-in.wav", .398107f);
        break;
    case Page::Crew:
        roomTab_ = 0;
        draftCard_ = appealCard_;
        draftStyle_ = boardStyle_;
        draftBase_ = boardBase_;
        draftStickers_ = placedStickers_;
        activeSticker_ = stickerFocus_ = -1;
        cardFilter_ = 0;
        favoriteCardsOnly_ = false;
        rebuildCards();
        decorationOffset_ = 0;
        crewLeavingAt_ = -1;
        crewChangedAt_ = now_;
        crewButton_ = -1;
        crewButtonAt_ = -1;
        crewPreview_ = selectedCrew_;
        keepCrewVisible();
        break;
    case Page::Songs:
        render_.queueAfp("next_area", "loading", "loading_in");
        if (newTrack && !(transition_ && transition_->newTrack)) cue = trackIntroCue();
        break;
    case Page::Matching: if (previous != Page::Settings) cue = Matching; break;
    case Page::Summary:
        cue = Total;
        summaryCountDone_ = summaryCountPlaying_ = false;
        skillChangePlayed_ = false;
        skillChangeEndPlayed_ = false;
        skillFailureSpoken_ = false;
        skillTitleHandled_ = false;
        skillTitleAt_ = skillTitleFocusAt_ = skillTitleDecidedAt_ = -1;
        skillBonusPhase_ = SkillBonusPhase::Pending;
        if (activeCourse_) {
            // 1805ab880 starts bank 24 slot 1 on success and slot 2 on failure.
            play(skillPassed() ? "skill_effects/audio/analysis-success.wav" : "skill_effects/audio/analysis-fail.wav",
                 .398107f);
        } else {
            play("result_force/audio/total_build.wav");
        }
        break;
    case Page::Exit:
        exitDecidedAt_ = 0;
        if (continuing_) cue = Continue;
        break;
    case Page::Saved: cue = Saved; break;
    case Page::GameEnd:
        play("entry_flow/ending.wav");
        // 1805012d7 chooses between end05 and end06 when the ending begins.
        cue = SDL_rand(2) ? Farewell : FarewellAlternate;
        break;
    case Page::Title: case Page::Stage: cue = Hidden; break;
    default: break;
    }
    if (page_ == Page::Result) {
        resultShineDuration_ = resultShine_.empty() ? 0 : render_.afpDuration("result_grade", resultShine_, resultShineClip());
        navigator_.result(result_, resultChangedLamp_, selectedCrew_, now_, resultShineDuration_);
        if (resultShineDuration_ > 0) play(resultGradeSound_);
    }
    else if (page_ == Page::Room && previous == Page::Credit) navigator_.enqueue(cue, now_);
    else if (page_ != Page::Pin && page_ != Page::Settings && page_ != Page::Credit && page_ != Page::Crew
        && !(page_ == Page::Mode && previous == Page::Credit)
        && !(page_ == Page::Songs && newTrack && transition_ && transition_->newTrack)
        && !(page_ == Page::Caution && previous == Page::Room)
        && !(page_ == Page::Room && previous == Page::Crew)) navigator_.play(cue, selectedCrew_, now_);
    if (page_ == Page::Settings) {
        optionFocus_ = 0;
        optionPopup_ = -1;
    }
}

void Cabinet::startTrackSelection() {
    if (sessionType_ == StartType::Premium && practiceUntil_ == 0) practiceUntil_ = now_ + 600000;
    if (sessionType_ == StartType::Skill) go(Page::Skill);
    else go(Page::Songs, true);
}

std::string_view Cabinet::trackIntroName() const {
    if (sessionType_ == StartType::Premium) return "in_premium";
    if (exTrack()) return "in_ex";
    return track_ == 1 ? "in_1st" : track_ == 2 ? "in_2nd" : "in_final";
}

NavigatorCue Cabinet::trackIntroCue() const {
    if (sessionType_ == StartType::Premium) return NavigatorCue::PremiumStart;
    if (exTrack()) return NavigatorCue::ExtraTrack;
    return track_ == 1 ? NavigatorCue::FirstTrack : track_ == 2 ? NavigatorCue::SecondTrack : NavigatorCue::FinalTrack;
}

void Cabinet::go(Page next, bool track) {
    if (transition_) return;
    if (page_ == Page::Pin && next == Page::Title)
        navigator_.play(NavigatorCue::Silent, selectedCrew_, now_);
    messageUntil_ = 0;
    achievementPopupAt_ = -1;
    if (page_ == Page::Settings) {
        if (lanePreview_) {
            lanePreview_.reset();
            audio_.stopPreview(true);
            saveProfile();
        }
        page_ = settingsReturn_;
        entered_ = coveredAt_;
        settingsLeavingAt_ = -1;
        nextSettingsTab_.reset();
        bindTarget_ = -1;
        optionPopup_ = -1;
        searchEditing_ = false;
        resetInput();
    }
    if ((page_ == Page::Songs || page_ == Page::Skill || page_ == Page::Result) && next == Page::Matching) {
        if (page_ != Page::Result) quickRetries_ = 0;
        render_.queueAfp("stage_ready", "ready", "matching_out");
        nextAreaLabelsReady_ = false;
        transition_ = Transition{next, now_, render_.afpDuration("next_area", "loading", "loading_in"),
            1800, render_.afpDuration("next_area", "loading", "loading_out"), TransitionKind::NextArea};
        audio_.stopPreview();
        if (page_ == Page::Songs) {
            audio_.stopMusic();
            activeMusic_.clear();
        }
        previewAt_ = -1;
        play("flow/se_08_big03_musicselect_nextarea.wav");
        SDL_Log("SDVX NEXT AREA %s track=%d", chart().track.title.c_str(), track_);
        return;
    }
    if (page_ == Page::Matching && next == Page::Stage) {
        const auto panelOut = characterView_ ? 0. : render_.afpDuration("matching", "maching_ue", "mat_comp");
        transition_ = Transition{next, now_, panelOut, 0, render_.afpDuration("stage_ready", "ready", "matching_out"),
            TransitionKind::MatchingOut, TransitionPhase::WaitingForStage};
        navigator_.play(NavigatorCue::Leave, selectedCrew_, now_);
        pendingSelection_ = chart().track;
        return;
    }
    if (page_ == Page::Matching) navigator_.play(NavigatorCue::Leave, selectedCrew_, now_);
    const auto announcement = trackIntroName();
    const double announcementIn = track ? render_.afpDuration("track_intro", announcement, "in") : 0;
    const double announcementOut = track ? render_.afpDuration("track_intro", announcement, "out") : 0;
    const bool intro = announcementIn > 0 && announcementOut > 0;
    const bool loading = track && track_ == 1 && !intro;
    const auto folder = loading ? "track_entry" : "shutter_nabla";
    const auto name = loading ? "loading" : "shutter_default";
    // CAUTION already covers the old scene. RESULT needs to close the cabinet
    // shutter before the song wheel and its announcement can appear together.
    const bool alreadyCovered = page_ == Page::Caution && (intro || next == Page::Skill);
    const double in = alreadyCovered ? 0 : render_.afpDuration(folder, name, loading ? "loading_in" : "in");
    const double out = render_.afpDuration(folder, name, loading ? "loading_out" : "out");
    if ((in > 0 || alreadyCovered) && out > 0) {
        if (page_ == Page::Title && next == Page::Pin && menuWarmup_ != MenuWarmup::Queued)
            now_ = std::max(now_, applicationTime());
        const auto kind = intro ? TransitionKind::TrackIntro : loading ? TransitionKind::TrackEntry : TransitionKind::Shutter;
        transition_ = Transition{next, now_, in, loading ? 640. : 0.,
            intro ? std::max(out, announcementIn + announcementOut) : out, kind, TransitionPhase::Covering, track};
        if (intro) {
            transition_->announcementInMs = announcementIn;
            render_.queueAfp("track_intro", announcement, "in");
            render_.queueAfp("track_intro", announcement, "out");
        }
        audio_.stopPreview();
        previewAt_ = -1;
        if (!alreadyCovered) play(loading ? TrackIn : ShutterIn);
        if (track && !intro) navigator_.play(trackIntroCue(), selectedCrew_, now_);
    } else {
        enter(next, track);
        if (next == Page::Stage) pendingSelection_ = chart().track;
    }
}

void Cabinet::update(double nowMs) {
    const double ticks = std::max(0., nowMs - now_) * .12;
    now_ = nowMs;
    if (startup_) {
        if (startup_->started < 0) return;
        if (startup_->catalogs.valid() && startup_->catalogs.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            startup_->catalogs.get();
            prepareProfile();
            openDocument();
            startup_->profileReady = true;
        }
        const double elapsed = now_ - startup_->started;
        if (!startup_->logoPlayed && elapsed >= KonamiLogoAt) {
            startup_->logoPlayed = true;
            audio_.sfx(KonamiSound, 1, 1);
        }
        if (elapsed >= StartupEnd && displayReady_ && startup_->profileReady && menuWarmup_ == MenuWarmup::Queued) {
            startup_.reset();
            enter(Page::Title);
        }
        return;
    }
    // Voice and motion continue while the page is covered or waiting for layout.
    navigator_.update(now_);
    if (!displayReady_) return;
    if (page_ == Page::Songs) {
        const auto [position, count] = songListPosition();
        if (songScrollIndex_ < 0.f) songScrollIndex_ = float(position);
        // 180293740 follows the nearest index around the circular list at .3 per 120 Hz tick.
        float delta = position - songScrollIndex_;
        if (delta > count * .5f) delta -= count;
        if (delta < -count * .5f) delta += count;
        songScrollIndex_ = std::abs(delta) < .01f ? float(position)
            : std::fmod(songScrollIndex_ + delta * float(1. - std::pow(.7, ticks)) + count, float(count));
        // The AFP seek then averages the existing frame and the new target.
        const float frame = 1000.f * songScrollIndex_ / count;
        if (songScrollFrame_ < 0.f || std::abs(frame - songScrollFrame_) < .01f) songScrollFrame_ = frame;
        else songScrollFrame_ += (frame - songScrollFrame_) * float(1. - std::exp2(-ticks));
    }
    if (songPromptPending_ && navigator_.readyForVoice()) {
        if (songPromptAt_ < 0) songPromptAt_ = now_ + 500;
        if (now_ >= songPromptAt_) {
            songPromptPending_ = false;
            navigator_.play(NavigatorCue::SongPrompt, selectedCrew_, now_);
        }
    }
    if (lanePreview_) lanePreview_->update(audio_.previewPositionMs());
    const bool modeModelReady = page_ != Page::Pin || navigator_.prepareModel(selectedCrew_);
    if (page_ == Page::Pin && entryStep_ >= EntryStep::Loading) {
        if (!entryProfileReady_) {
            // big_1_wallet is 1080x500, with the 960x360 card inset by (60,70).
            render_.captureImage("runtime/entry-profile", 1080, 500, [&] {
                drawProfile({60,70,960,360}, false);
                const auto packet = render_.imageSize("entry_checkin/box_pc.png");
                const auto block = render_.imageSize("entry_checkin/box_bloc.png");
                render_.image("entry_checkin/box_pc.png", {679.25f,400,packet.x * .85f,packet.y * .85f});
                render_.image("entry_checkin/box_bloc.png", {842.25f,400,block.x * .85f,block.y * .85f});
                render_.text(std::to_string(packets_), 798, 424, 15, White, TextAlign::Right, 74);
                render_.text(std::to_string(blocks_), 961, 424, 15, White, TextAlign::Right, 74);
                if (blasterPassActive()) ap("entry_checkin", "blpass_active", "0", 68, 34, 0);
            });
            entryProfileReady_ = true;
        }
        if (!modeLabelsReady_) prepareModeArtwork();
    }
    if (page_ == Page::Crew && (roomTab_ == 2 || roomTab_ == 3)) {
        const std::array state{draftCard_,draftStyle_,draftBase_,selectedCrew_,totalForce()};
        if (state != profileCacheState_ || draftStickers_ != profileCacheStickers_) {
            render_.captureImage("runtime/profile-preview",960,360,[&] { drawProfile({0,0,960,360},true); });
            profileCacheState_ = state;
            profileCacheStickers_ = draftStickers_;
        }
    }
    if (page_ == Page::Room && crewSwitchPending_) {
        crewSwitchPending_ = false;
        crewOnMain_ = selectedCrew_;
        navigator_.play(NavigatorCue::CrewConfirmed, selectedCrew_, now_);
    }
    if (page_ == Page::Room && startAfterSubscreen_ && !navigator_.acting() && navigator_.readyForVoice()) {
        startAfterSubscreen_ = false;
        setRoomFocus(1);
        confirm();
        return;
    }
    if (transition_) {
        auto& transition = *transition_;
        if (transition.phase == TransitionPhase::WaitingForStage) return;
        if (transition.kind == TransitionKind::MatchingOut) {
            if (now_ - transition.started < transition.holdMs + transition.outMs) return;
            page_ = Page::Stage;
            navigator_.play(NavigatorCue::Hidden, selectedCrew_, now_);
            transition_.reset();
            return;
        }
        const double covered = transition.started + transition.inMs;
        const double hold = transition.holdMs;
        if (transition.phase == TransitionPhase::Covering) {
            if (now_ < covered + hold) return;
            enter(transition.next, transition.newTrack);
            if (transition.next == Page::Stage) {
                transition.phase = TransitionPhase::WaitingForStage;
                pendingSelection_ = chart().track;
                return;
            }
            transition.phase = TransitionPhase::Opening;
            if (transition.kind == TransitionKind::TrackIntro) {
                transition.started += now_ - covered - hold;
                play(TrackIn);
                navigator_.play(trackIntroCue(), selectedCrew_, now_);
                return;
            } else if (transition.kind != TransitionKind::NextArea) {
                play(transition.kind == TransitionKind::TrackEntry ? TrackOut : ShutterOut);
            }
        }
        if (transition.kind == TransitionKind::TrackIntro && transition.announcementOutOffset < 0
                && now_ >= covered + hold + transition.announcementInMs) {
            // Start the fade and its sound on the same frame even after a slow
            // song-wheel draw. Keep the entire authored out segment visible.
            transition.announcementOutOffset = now_ - covered - hold;
            transition.outMs = std::max(transition.outMs, transition.announcementOutOffset
                + render_.afpDuration("track_intro", trackIntroName(), "out"));
            play(TrackOut);
        }
        if (now_ < covered + hold + transition.outMs) return;
        const Page opened = transition.next;
        transition_.reset();
        if (opened == Page::Songs) startSongSelectionAudio();
        if (opened == Page::Pin) {
            entered_ = now_;
            entryStep(entryStep_);
        }
    }
    if (roomDepartureAt_ >= 0) {
        const auto decide = render_.afpDuration("room_flow", "cursor", "decide");
        const auto out = render_.afpDuration("room_flow", "room_1", "out");
        if (now_ - roomDepartureAt_ >= decide + out && !navigator_.acting()) {
            // 1804fd8b0 still closes the shutter after CONTINUE, but bypasses CAUTION.
            if (continuing_) startTrackSelection();
            else enter(Page::Caution);
        }
        return;
    }
    if (page_==Page::Songs && songOverlayOutAt_>=0) {
        const auto movie=sortOpen_?"sort":"window_select";
        if (now_-songOverlayOutAt_>=render_.afpDuration("selection_ui",movie,"out")) {
            const bool enterSong=songConfirm_ && songConfirmAccept_;
            if (songConfirm_ && visibleCharts_[songIndex_] >= 0 && !enterSong) chartIndex_ = visibleCharts_[songIndex_];
            if (songConfirm_ && visibleCharts_[songIndex_] < 0 && !exChallengeFolder()) preferredChart_ = chartIndex_;
            sortOpen_=songConfirm_=songConfirmAccept_=false; songOverlayOutAt_=-1;
            if (enterSong) { go(Page::Matching); return; }
        } else return;
    }
    if (page_ == Page::Settings && settingsLeavingAt_ >= 0
        && now_ - settingsLeavingAt_ >= settingsDuration("out")) {
        if (nextSettingsTab_) {
            settingsTab_ = *nextSettingsTab_;
            nextSettingsTab_.reset();
            optionFocus_ = 0;
            bindingFocus_ = 0;
            calibration_.fill(0);
            entered_ = now_;
        } else {
            page_ = settingsReturn_;
            entered_ = coveredAt_;
            if (page_ == Page::Songs && wheel_ == Wheel::Songs) previewAt_ = now_ + SongPreviewDelay;
        }
        settingsLeavingAt_ = -1;
        resetInput();
    }
    if (page_ == Page::Settings
        && ((settingsReturn_ == Page::Songs && sessionType_ != StartType::Premium && now_ - coveredAt_ >= SongTime)
            || (settingsReturn_ == Page::Matching && now_ - coveredAt_ >= MatchTime))) {
        if (lanePreview_) {
            lanePreview_.reset();
            audio_.stopPreview(true);
        }
        saveProfile();
        page_ = settingsReturn_;
        entered_ = coveredAt_;
        settingsLeavingAt_ = -1;
        nextSettingsTab_.reset();
        bindTarget_ = -1;
        optionPopup_ = -1;
        resetInput();
    }
    const double elapsed = now_ - entered_;
    if (page_ == Page::Title && titleCallPending_ && elapsed >= TitleCallAt) {
        titleCallPending_ = false;
        navigator_.play(alternateTitleCall_ ? NavigatorCue::TitleCallSecond : NavigatorCue::TitleCallFirst,
                        selectedCrew_, now_);
        alternateTitleCall_ = !alternateTitleCall_;
    }
    if (page_ == Page::Generator && generatorPhase_ >= 2) {
        const auto name = generatorPhase_ == 2 ? "anime_gene" : "parts_get";
        if (now_ - generatorAt_ >= render_.afpDuration("generator_vii", name, "in")) {
            generatorAt_ = now_;
            if (generatorPhase_ == 2) {
                generatorPhase_ = 3;
                play("generator_vii/get.wav");
            } else if (++generatorRewardIndex_ < generatorRewards_.size()) {
                generatorPhase_ = 2;
                play("generator_vii/roll.wav");
            } else {
                generatorPhase_ = 0;
                generatorRewards_.clear();
                generatorCount_ = std::clamp(generatorCount_, 1, std::max(1, packets_ / GeneratorPrice));
            }
        }
        return;
    }
    if (page_ == Page::Pin) {
        if (pinVerification_) {
            if (pinVerification_->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const bool accepted = pinVerification_->get();
            pinVerification_.reset();
            if (pinPhase_ == PinPhase::Verify) {
                audio_.stopEffectLoop();
                const float gainDb = accepted ? -5.3984375f : -6.09765625f;
                play(accepted ? "entry_original/audio/se_01_short003.wav" : "entry_original/audio/se_01_short004.wav",
                     std::pow(10.f, gainDb / 20.f));
            }
            const auto elapsedCheck = now_ - pinVerificationAt_;
            // Verification stays on the settled PIN panel, with its timeout paused.
            entered_ += elapsedCheck;
            entryAt_ += elapsedCheck;
            if (accepted) {
                pinVerificationAt_ = now_;
                pendingPin_.clear();
                entryAuthenticated_ = true;
                saveProfile();
                if (pinPhase_ == PinPhase::Verify) navigator_.play(NavigatorCue::PinAccepted, selectedCrew_, now_);
            } else {
                pinError_ = pinPhase_ == PinPhase::Verify ? "暗証番号が違います" : "暗証番号を保存できませんでした";
                navigator_.play(pinPhase_ == PinPhase::Verify ? NavigatorCue::PinRejected : NavigatorCue::Silent,
                                selectedCrew_, now_);
            }
            return;
        }
        if (entryNext_) {
            const auto out = render_.afpDuration(entryFolder(), entryMovie(), "out");
            if (now_ - entryOutAt_ >= out) {
                const auto next = *entryNext_;
                if (next == EntryStep::ModeSelect) {
                    if (modeModelReady) enter(Page::Mode);
                    return;
                }
                entryStep(next);
            }
            return;
        }
        const double age = now_ - entryAt_;
        if (entryStep_ == EntryStep::Pin && pinPhase_ == PinPhase::Verify && !entryAuthenticated_) {
            const int remaining = static_cast<int>(std::ceil(std::max(0., PinTime - age) / 1000));
            if (remaining != pinCountdown_) {
                pinCountdown_ = remaining;
                if (remaining <= 10) audio_.sfx("entry_original/audio/s205_timer_countdown.wav", twoDxGain(109), 1);
            }
        }
        if (entryStep_ == EntryStep::CardRead && age >= EntryConfirmHold)
            advanceEntry(pinPhase_ == PinPhase::Create ? EntryStep::Registration : EntryStep::Pin);
        // 1804deaa0 uses the 180-tick confirmation gate in 18030b6a0.
        else if (entryStep_ == EntryStep::Pin && entryAuthenticated_ && now_ - pinVerificationAt_ >= EntryConfirmHold)
            advanceEntry(entryNeedsName_ ? EntryStep::Code : EntryStep::Loading);
        else if (entryStep_ == EntryStep::Loading && entryProfileReady_) {
            if (entryLoadingVoiceAt_ < 0) {
                if (age >= render_.afpDuration(entryFolder(), entryMovie(), "in") && navigator_.readyForVoice()) {
                    entryLoadingVoiceAt_ = now_;
                    navigator_.play(NavigatorCue::EntryLoading, selectedCrew_, now_);
                }
            } else if (now_ - entryLoadingVoiceAt_ >= EntryLoadingVoiceHold) {
                advanceEntry(EntryStep::Profile);
            }
        }
        else if (entryStep_ == EntryStep::Profile && age >= 10000) confirmEntry();
        // The local balance is already available when the gray check-in window settles.
        else if (entryStep_ == EntryStep::PaymentCheck && age >= render_.afpDuration(entryFolder(), entryMovie(), "in") + 100)
            entryStep(EntryStep::PaymentNotice);
        else if (entryStep_ == EntryStep::PaymentNotice && age >= PaymentNoticeTime)
            advanceEntry(EntryStep::ModeSelect);
        else if (!entryAuthenticated_ && age >= (entryNeedsName_ ? 180000 : PinTime)) go(Page::Title);
        return;
    }
    if (page_ == Page::Caution) {
        if (!cautionOpened_ && elapsed >= CautionOpenSoundAt) {
            cautionOpened_ = true;
            play(EntryOpenHigh);
        }
        if (!cautionClosing_ && elapsed >= CautionCloseSoundAt) {
            cautionClosing_ = true;
            play(EntryCloseHigh);
        }
    }
    if (page_ == Page::Credit && paymentAcceptedAt_ >= 0) {
        const auto menuEnd = paymentAcceptedAt_ + render_.afpDuration("payment", "paseli_credit", "out");
        if (now_ < menuEnd) return;
        if (paymentCanceled_) {
            enter(Page::Mode);
            return;
        }
        if (paymentReceiptPrice_ > 0) {
            if (paymentReceiptAt_ < 0) {
                paymentReceiptAt_ = now_;
                play("payment/audio/paseli-ok.wav");
            } else if (paymentReceiptOutAt_ < 0 && now_ - paymentReceiptAt_ >= PaymentReceiptTime) {
                paymentReceiptOutAt_ = now_;
                play(EntryCloseLow);
            }
        }
        const double modeOutAt = paymentModeOutAt();
        if (modeOutAt >= 0 && now_ - modeOutAt >= render_.afpDuration("mode_flow", "mode_current", "out"))
            enter(Page::Room);
        return;
    }
    if (page_ == Page::Exit) {
        if (exitDecidedAt_ > 0) {
            const double closing = premiumExit() ? render_.afpDuration("session_dialog", "win_yes_no", "out") : 0;
            if (now_ - exitDecidedAt_ >= 420 + closing && (!continuing_ || navigator_.readyForVoice())) {
                if (continuing_) {
                    if (exitFocus_ == 0) go(Page::Mode);
                    else enter(Page::Saved);
                } else if (exitFocus_ == 0) {
                    if (exitReturn_ == Page::Songs && !sessionResults_.empty()) go(Page::Summary);
                    else enter(Page::Saved);
                } else {
                    enter(exitReturn_);
                    entered_ = now_ - exitElapsed_;
                }
            }
            return;
        }
        if (continuing_ && elapsed >= ContinueTime) {
            exitFocus_ = 1;
            confirm();
            return;
        }
    }
    if (page_ == Page::Result && !forceDone_) {
        if (forceAt_ < 0 && !resultVoicePending()) startResultForce();
        if (forceAt_ >= 0) {
            if (forceEffect_ == 2 && !forceLevelSound_ && now_ - forceAt_ >= 1250) {
                forceLevelSound_ = true;
                play("result_force/audio/level.wav");
            }
            const double duration = forceEffect_ == 3 ? 652000./120 : forceEffect_ == 2 ? 2000 : 146000./120;
            if (now_ - forceAt_ >= duration) forceDone_ = true;
        }
    }
    if (page_ == Page::Result && forceDone_ && !resultVoicePending() && resultRewardsAt_ < 0 &&
        elapsed >= render_.afpDuration("result_original", "result_all_default", "in"))
        resultRewardsAt_ = now_;
    if (page_ == Page::Summary) {
        const double resultAge = elapsed - skillResultIntroDuration();
        if (activeCourse_ && !skillPassed() && !skillFailureSpoken_ && resultAge >= 0) {
            skillFailureSpoken_ = true;
            navigator_.enqueue(NavigatorCue::SkillFailed, now_);
        }
        if (skillLevelChanged_ && !skillChangePlayed_ && resultAge >= SkillChangeAt) {
            skillChangePlayed_ = true;
            navigator_.enqueue(NavigatorCue::SkillLevelUp, now_);
            play("skill_effects/audio/skill-change.wav", 1.122018f);
        }
        const double changeEnd = skillLevelChanged_ ? SkillChangeAt + render_.afpDuration("skill_effects", "skill_change", "in") : 0;
        if (skillLevelChanged_ && !skillChangeEndPlayed_ && resultAge >= changeEnd) {
            skillChangeEndPlayed_ = true;
            play("skill_effects/audio/skill-change-end.wav");
        }
        if (!summaryCountDone_) {
            if (resultAge >= 2200) {
                finishSummaryCount();
            } else if (!summaryCountPlaying_ && (sessionPackets_ || sessionBlocks_) &&
                       resultAge >= render_.afpDuration("total_result", "total_all", "in")) {
                audio_.startEffectLoop(CountMove, twoDxGain(48));
                summaryCountPlaying_ = true;
            }
        }
        if (activeCourse_ && skillPassed() && !skillTitleHandled_ && resultAge >= changeEnd && summaryCountDone_) {
            skillTitleHandled_ = true;
            const auto& course = courses_[*activeCourse_];
            if (skillTitle_ == 0) {
                skillTitle_ = course.nameId;
                saveProfile();
            } else if (skillTitle_ != course.nameId) {
                skillTitleAt_ = now_;
                skillTitleFocus_ = 0;
            }
        }
        if (skillTitleAt_ >= 0) {
            if (skillTitleDecidedAt_ < 0) {
                const double windowIntro = render_.afpDuration("skill_effects/windows", "win_skill_change", "in");
                if (now_ - skillTitleAt_ >= windowIntro + SkillTitleTime) confirm();
            } else if (
                now_ - skillTitleDecidedAt_ >= render_.afpDuration("session_dialog", "yes_no", "decide_yes")
                    + render_.afpDuration("skill_effects/windows", "win_skill_change", "out")) {
                skillTitleAt_ = -1;
            }
            if (skillTitleAt_ >= 0) return;
        }
        if (activeCourse_ && skillPassed() && skillTitleHandled_ && skillBonusPhase_ == SkillBonusPhase::Pending) {
            const auto& course = courses_[*activeCourse_];
            const auto key = std::pair{course.seasonId, course.id};
            if (std::find(clearedSkillCourses_.begin(), clearedSkillCourses_.end(), key) != clearedSkillCourses_.end()) {
                skillBonusPhase_ = SkillBonusPhase::Done;
            } else {
                // 1805b0040/1805b0570: the default award is indexed by previously
                // cleared courses in this season, and requires a new clear.
                const auto cleared = std::count_if(clearedSkillCourses_.begin(), clearedSkillCourses_.end(),
                    [&](const auto& record) { return record.first == course.seasonId; });
                skillBonus_ = std::array{512, 256, 128}[std::min<decltype(cleared)>(cleared, 2)];
                skillBonusPhase_ = SkillBonusPhase::Waiting;
                skillBonusAt_ = now_;
                play("skill_effects/audio/bonus-in.wav", .707946f);
            }
        }
        const double bonusAge = now_ - skillBonusAt_;
        switch (skillBonusPhase_) {
        case SkillBonusPhase::Waiting:
            if (bonusAge >= render_.afpDuration("skill_effects/windows", "win_skill_bonus", "in") + SkillBonusWait) confirm();
            return;
        case SkillBonusPhase::Counting:
            if (!skillBonusRemaining(skillBonus_) && !skillBonusRemaining(skillBonusPackets_) &&
                !skillBonusRemaining(skillBonusBlocks_)) {
                audio_.stopEffectLoop();
                play(CountEnd, twoDxGain(43));
                skillBonusPhase_ = SkillBonusPhase::Ready;
                skillBonusAt_ = now_;
            }
            return;
        case SkillBonusPhase::Ready:
            if (bonusAge >= SkillBonusResultWait) confirm();
            return;
        case SkillBonusPhase::Closing:
            if (bonusAge >= render_.afpDuration("skill_effects/windows", "win_skill_bonus", "out")) {
                skillBonusPhase_ = SkillBonusPhase::Done;
                confirm();
            }
            return;
        default: break;
        }
    }
    if (page_ == Page::Result && resultRetryAt_ >= 0 && !practiceExpired()
        && now_ - resultRetryAt_ >= PremiumRetryHoldMs) {
        resultRetryAt_ = -1;
        go(Page::Matching);
        return;
    }
    if ((page_ == Page::Caution && elapsed >= render_.afpDuration("caution_original","caution","0"))
        || (page_ == Page::Result && elapsed >= ResultTime) || (page_ == Page::Summary && elapsed >= SummaryTime)
        || (page_ == Page::Saved && elapsed >= 4200 && navigator_.readyForVoice())
        || (page_ == Page::GameEnd && elapsed >= 8000 && navigator_.readyForVoice())) {
        confirm();
        return;
    }
    if (practiceExpired() && (page_ == Page::Songs || page_ == Page::Matching
        || page_ == Page::Settings || page_ == Page::Exit)) {
        go(sessionResults_.empty() ? Page::Saved : Page::Summary);
        return;
    }
    if (page_ == Page::Songs && sessionType_ != StartType::Premium && elapsed >= SongTime) {
        if (sortOpen_) { closeSongOverlay(false); return; }
        if (songConfirm_) { confirm(); return; }
        if (activeFolder_ < 0) {
            rebuildFolders();
            activeFolder_ = 0;
            rebuildSongs();
        }
        wheel_ = Wheel::Songs;
        std::vector<std::pair<int,int>> playable;
        for (int index = 0; index < int(visibleSongs_.size()); ++index) {
            const auto& entry = songs_[visibleSongs_[index]];
            const auto [begin, end] = chartRange(entry, exChallengeFolder());
            const int fixed = visibleCharts_[index];
            for (int chart = begin; chart < end; ++chart)
                if (entry.charts[chart].installed && (fixed < 0 || fixed == chart))
                    playable.emplace_back(index, chart);
        }
        if (playable.empty()) {
            rebuildFolders(); activeFolder_=0;
            rebuildSongs();
            return;
        }
        const auto selected = exChallengeFolder() ? std::pair{songIndex_, chartIndex_}
            : playable[SDL_rand(int(playable.size()))];
        songIndex_ = selected.first;
        chartIndex_ = selected.second;
        SDL_Log("SDVX song timeout selected %s %s", chart().track.folder.c_str(), chart().track.difficulty.c_str());
        confirm();
        return;
    }
    audio_.updatePreview();
    if (page_ == Page::Songs && previewAt_ >= 0) {
        const auto& selected = chart();
        if (selected.installed && !selected.track.preview.empty() && selected.track.id != AutomationParadise2019) {
            const auto& track = selected.track;
            const auto path = track.asset(track.preview);
            // Decode during the original selection delay so a compressed preview does not block rendering.
            if (audio_.previewReady(path, track.externalPreview) && now_ >= previewAt_) {
                previewAt_ = -1;
                audio_.preview(path, .82f, false, track.externalPreview);
            }
        } else if (now_ >= previewAt_) {
            previewAt_ = -1;
            audio_.stopPreview(true);
        }
    }
    if (page_ == Page::Matching) {
        if (!matchingSound_ && now_ - entered_ >= 350) {
            matchingSound_ = true;
            play("flow/se_08_big02_matching_motion.wav");
        }
        if (now_ - entered_ >= MatchTime) confirm();
    }
    if (page_ == Page::Crew && crewLeavingAt_ >= 0) {
        // Prepare the new model during the submonitor's exit so a cold load
        // cannot consume the portrait entrance or delay it behind its voice.
        const bool crewReady = !crewSwitchPending_ || navigator_.prepareModel(selectedCrew_);
        if (crewReady && now_ - crewLeavingAt_ >=
                render_.afpDuration(roomTab_ == 1 ? "crew_selector" : "subscreen", roomPageName(), "out"))
            enter(Page::Room);
    }
}

void Cabinet::setDisplayReady(bool ready, double now) {
    if (!ready && displayReady_) {
        displayWaitingAt_ = now;
        resetInput();
    } else if (ready && !displayReady_) {
        // Orientation changes are asynchronous. Start the submonitor's authored
        // entrance only after its surface is visible; session clocks keep running.
        if (landscape()) entered_ += now - displayWaitingAt_;
        if (transition_) transition_->started += now - displayWaitingAt_;
        displayWaitingAt_ = -1;
    }
    displayReady_ = ready;
}

std::optional<Selection> Cabinet::takeSelection() {
    auto next = std::move(pendingSelection_);
    pendingSelection_.reset();
    return next;
}

const Selection* Cabinet::stagePreparation() const {
    const bool matching = page_ == Page::Matching
        || (page_ == Page::Settings && settingsReturn_ == Page::Matching);
    if (transition_ ? transition_->next == Page::Matching : matching) return &chart().track;
    return nullptr;
}

bool Cabinet::stageTransition() const {
    return transition_ && transition_->next == Page::Stage;
}

bool Cabinet::stageDepartureComplete(double nowMs) const {
    return !matchingDeparture() || nowMs - transition_->started >= transition_->inMs;
}

bool Cabinet::stageClosing(double nowMs) const {
    return page_ == Page::Stage && transition_ && transition_->phase == TransitionPhase::Covering
        && nowMs < transition_->started + transition_->inMs + transition_->holdMs;
}

void Cabinet::stageLoaded(double nowMs) {
    if (!stageTransition() || transition_->phase != TransitionPhase::WaitingForStage) return;
    auto& shutter = *transition_;
    shutter.phase = TransitionPhase::Opening;
    if (shutter.kind == TransitionKind::MatchingOut) {
        shutter.holdMs = nowMs - shutter.started;
        play("flow/se_08_big02_matching_ready2.wav");
        return;
    }
    shutter.started = nowMs - shutter.inMs - shutter.holdMs;
    play(ShutterOut);
}

void Cabinet::applyStageOptions(const PlayOptions& options) {
    const bool retryChanged = sessionType_ == StartType::Premium && options_.autoRetry != options.autoRetry;
    if (options_.speed == options.speed && options_.displayOffset == options.displayOffset
        && options_.judgmentOffset == options.judgmentOffset && options_.fineTiming == options.fineTiming && !retryChanged) return;
    options_.speed = stageOptions_.speed = options.speed;
    options_.displayOffset = stageOptions_.displayOffset = options.displayOffset;
    options_.judgmentOffset = stageOptions_.judgmentOffset = options.judgmentOffset;
    options_.fineTiming = stageOptions_.fineTiming = options.fineTiming;
    if (sessionType_ == StartType::Premium) options_.autoRetry = stageOptions_.autoRetry = options.autoRetry;
    saveProfile();
}

void Cabinet::stageFinished(const Result& result, double nowMs) {
    // A stage hands its result over once, before the closing shutter starts.
    if (page_ != Page::Stage || transition_) return;
    now_ = nowMs;
    if (result.cancelled) {
        if (activeCourse_) {
            result_ = result;
            go(Page::Summary);
        } else go(practiceExpired() ? (sessionResults_.empty() ? Page::Saved : Page::Summary) : Page::Songs);
        return;
    }
    result_ = result;
    resultBlasterBefore_ = blasterEnergy_;
    const auto& track = chart().track;
    const auto previous = record(track);
    previousBest_ = previous.score;
    forceBefore_ = totalForce();
    forceAt_ = -1;
    forceLevelSound_ = false;
    resultShine_ = {};
    resultGradeSound_ = {};
    // 1803ad6d0 compares the stored grade against the threshold of the new grade.
    constexpr std::array<std::string_view, 3> grades{"s", "aaa_plus", "aaa"};
    constexpr std::array<std::string_view, 3> shines{"shining_s", "shining_aaa_plus", "shining_aaa"};
    constexpr std::array<std::string_view, 3> sounds{
        "result_original/audio/se_06_rank001_s.wav",
        "result_original/audio/se_06_rank002_aaa_plus.wav",
        "result_original/audio/se_06_rank003_aaa.wav",
    };
    const auto rank = std::find(grades.begin(), grades.end(), result.grade);
    if (rank < std::find(grades.begin(), grades.end(), previous.grade)) {
        const auto index = rank - grades.begin();
        resultShine_ = shines[index];
        resultGradeSound_ = sounds[index];
        render_.queueAfp("result_grade", resultShine_, resultShineClip());
    }
    if (result.autoPlay) {
        resultChangedLamp_ = false;
        resultBlocks_ = {};
    } else {
        auto& best = records_[keyFor(track)];
        const std::string_view lamp = result.outcome != "comp" ? std::string_view(result.outcome)
            : result.ultra ? "comp_ultra" : result.excessive ? "comp_ex" : "comp";
        resultChangedLamp_ = lampRank(lamp) > lampRank(best.outcome);
        if (result.score >= previousBest_) {
            best.score = result.score;
            best.grade = result.grade;
        }
        if (lampRank(lamp) >= lampRank(best.outcome)) best.outcome = lamp;
        best.force = std::max(best.force, chartForce(track.level, result));
        const auto radar = playerRadar(chart(), result.score, result.exScore, result.maxExScore);
        for (int axis = 0; axis < int(radar.size()); ++axis)
            best.radar[axis] = std::max(best.radar[axis], radar[axis]);
        resultBlocks_ = stageBlocks(track.level, result, sessionPaidWithPaseli_, 99999999 - blocks_);
        blocks_ += resultBlocks_.total();
        sessionBlocks_ += resultBlocks_.total();
        // 1803b70a0 and the recording: Standard and a completed Premium track
        // contribute 867 hundredths. BLASTER PASS doubles the gain.
        if (sessionType_ == StartType::Standard || sessionType_ == StartType::Premium)
            blasterEnergy_ = std::min(MaxBlasterEnergy, blasterEnergy_ + 867 * (blasterPassActive() ? 2 : 1));
        if (exTrack() && exChallengeFolder() && lampRank(result.outcome) >= lampRank("comp"))
            exChallengeUnlocks_[song().challenge] = chartIndex_ + 1;
        songFoldersDirty_ = true;
        updateAchievements();
    }
    forceAfter_ = result.autoPlay ? forceBefore_ : totalForce();
    forceEffect_ = forceEffect(forceBefore_, forceAfter_);
    forceDone_ = forceEffect_ == 0;
    sessionResults_.push_back({track, result});
    if (!result.autoPlay) {
        if (skillPassed() && courses_[*activeCourse_].level > skillLevel_) {
            skillLevel_ = courses_[*activeCourse_].level;
            skillLevelChanged_ = true;
        }
        saveProfile();
    }
    go(Page::Result);
}

void Cabinet::credit() {
    if (credits_ < 99) {
        ++credits_;
        play("flow/coin.wav", 1);
    }
}

std::span<const Cabinet::PaymentOffer> Cabinet::modePayments(int mode) {
    // 180252610 lists the rows and 180254bd0 resolves the price event.
    // The local cabinet uses the recorded rates and keeps network-only modes as previews.
    static constexpr std::array normal{
        PaymentOffer{"6_pay_text_01a", 206, true, StartType::Light},
        PaymentOffer{"6_pay_text_01b", 267, true, StartType::Standard},
        PaymentOffer{"6_pay_text_01a", 2, false, StartType::Light}
    };
    static constexpr std::array skill{
        PaymentOffer{"6_pay_text_01a", 206, true, StartType::Skill},
        PaymentOffer{"6_pay_text_01b", 267, true, StartType::Skill},
        PaymentOffer{"6_pay_text_01a", 2, false, StartType::Skill}
    };
    static constexpr std::array premium{PaymentOffer{"6_pay_text_01e", 411, true, StartType::Premium}};
    static constexpr std::array blaster{PaymentOffer{"6_pay_text_01d", 411, true, std::nullopt}};
    static constexpr std::array megamix{
        PaymentOffer{"6_pay_text_01f", 206, true, std::nullopt},
        PaymentOffer{"6_pay_text_01f", 2, false, std::nullopt}
    };
    static constexpr std::array arena{
        PaymentOffer{"6_pay_text_01h", 206, true, std::nullopt},
        PaymentOffer{"6_pay_text_01h", 267, true, std::nullopt},
        PaymentOffer{"6_pay_text_01h", 2, false, std::nullopt}
    };
    switch (mode) {
    case 0: return blaster;
    case SkillMode: return skill;
    case PremiumMode: return premium;
    case NormalMode: return normal;
    case 5: return megamix;
    case 6: case 7: return arena;
    default: return {};
    }
}

void Cabinet::startSession(const PaymentOffer& offer) {
    if (!offer.session || (offer.session == StartType::Skill && courses_.empty())) {
        notice("このモードは現在利用できません");
        return;
    }
    int& balance = offer.paseli ? paseli_ : credits_;
    if (balance < offer.price) {
        notice(offer.paseli ? "PASELI が不足しています" : "CREDIT が不足しています");
        return;
    }
    balance -= offer.price;
    ++playCount_;
    const auto day = localDay(), week = (day + 3) / 7;
    if (day != lastPlayDay_) {
        dayChain_ = day == lastPlayDay_ + 1 ? dayChain_ + 1 : 1;
        ++playDays_;
        todayPlays_ = 0;
        lastPlayDay_ = day;
    }
    if (week != lastPlayWeek_) {
        weekChain_ = week == lastPlayWeek_ + 1 ? weekChain_ + 1 : 1;
        lastPlayWeek_ = week;
    }
    ++todayPlays_;
    updateAchievements();
    saveProfile();
    sessionType_ = *offer.session;
    sessionPaidWithPaseli_ = offer.paseli;
    paymentReceiptPrice_ = offer.paseli ? offer.price : 0;
    track_ = 1;
    activeCourse_.reset();
    skillLevelBefore_ = skillLevel_;
    skillTitleBefore_ = skillTitle_;
    skillLevelChanged_ = false;
    practiceUntil_ = 0;
    sessionResults_.clear();
    sessionPackets_ = sessionBlocks_ = 0;
    wheel_ = Wheel::Songs; rebuildFolders(); activeFolder_=0; rebuildSongs();
    play(Decide);
    SDL_Log("SDVX session started type=%d credits=%d", static_cast<int>(sessionType_), credits_);
    paymentAcceptedAt_ = now_;
    play(EntryCloseLow);
    resetInput();
}

double Cabinet::paymentModeOutAt() const {
    if (paymentAcceptedAt_ < 0 || paymentCanceled_) return -1;
    if (paymentReceiptPrice_ == 0)
        return paymentAcceptedAt_ + render_.afpDuration("payment", "paseli_credit", "out");
    if (paymentReceiptOutAt_ < 0) return -1;
    return paymentReceiptOutAt_ + render_.afpDuration("entry_checkin", "win_paseli_green", "out");
}

bool Cabinet::canEndPremium() const {
    return page_ == Page::Songs && sessionType_ == StartType::Premium && practiceUntil_ > now_
        && !transition_ && !sortOpen_ && !songConfirm_;
}

bool Cabinet::premiumExit() const {
    return page_ == Page::Exit && !continuing_ && exitReturn_ == Page::Songs && sessionType_ == StartType::Premium;
}

void Cabinet::requestPremiumExit() {
    continuing_ = false;
    exitReturn_ = Page::Songs;
    exitElapsed_ = now_ - entered_;
    exitFocus_ = 1;
    enter(Page::Exit);
    play(EntryOpenLow);
}

std::string_view Cabinet::entryMovie() const {
    switch (entryStep_) {
    case EntryStep::CardRead:
    case EntryStep::Loading: return "win_connect";
    case EntryStep::Registration: return "entry_03";
    case EntryStep::Transfer: return "entry_03_02";
    case EntryStep::Terms: return "entry_04";
    case EntryStep::Pin: return "entry_07";
    case EntryStep::Code: return "entry_06";
    case EntryStep::Name: return "entry_09";
    case EntryStep::Profile: return "entry_02_id";
    case EntryStep::PaymentCheck: return "win_paseli_gray";
    case EntryStep::PaymentNotice: return "win_paseli_yellow";
    case EntryStep::ModeSelect: break;
    }
    return "";
}

std::string_view Cabinet::entryFolder() const {
    switch (entryStep_) {
    case EntryStep::CardRead:
    case EntryStep::Loading: return "entry_wait";
    case EntryStep::Profile: return "entry_profile";
    case EntryStep::PaymentCheck:
    case EntryStep::PaymentNotice: return "entry_checkin";
    default: return "entry_original";
    }
}

void Cabinet::entryStep(EntryStep next) {
    entryStep_ = next;
    entryAt_ = now_;
    entryNext_.reset();
    entryOutAt_ = -1;
    entryFocus_ = 0;
    resetInput();
    if (next == EntryStep::Name) nameIndex_ = 0;
    if (next == EntryStep::Loading) {
        entryProfileReady_ = false;
        entryLoadingVoiceAt_ = -1;
        refreshBlasterPass();
    }
    if (next != EntryStep::PaymentNotice)
        play(next == EntryStep::CardRead || next == EntryStep::Loading ? EntryOpenLow : EntryOpenHigh);
    if (next == EntryStep::CardRead) navigator_.play(NavigatorCue::PinAccepted, selectedCrew_, now_);
    else if (next == EntryStep::Pin && pinPhase_ == PinPhase::Verify) {
        pinCountdown_ = static_cast<int>(PinTime / 1000);
        // 1804deaa0 queues bank index 13 when the PIN window opens.
        navigator_.enqueue(NavigatorCue::PinPrompt, now_);
    }
}

void Cabinet::advanceEntry(EntryStep next) {
    if (entryNext_) return;
    entryNext_ = next;
    entryOutAt_ = now_ + (entryStep_ == EntryStep::Registration ? 420 : 0);
    resetInput();
    play(entryStep_ == EntryStep::CardRead || entryStep_ == EntryStep::Loading ? EntryCloseLow : EntryCloseHigh);
}

void Cabinet::confirmEntry() {
    if (entryStep_ != EntryStep::Pin &&
        (entryNext_ || now_ - entryAt_ < render_.afpDuration(entryFolder(), entryMovie(), "in"))) return;
    switch (entryStep_) {
    case EntryStep::Registration: advanceEntry(entryFocus_ ? EntryStep::Transfer : EntryStep::Terms); break;
    case EntryStep::Transfer: advanceEntry(EntryStep::Registration); break;
    case EntryStep::Terms: advanceEntry(entryFocus_ ? EntryStep::Registration : EntryStep::Pin); break;
    case EntryStep::Pin:
        if (entryAuthenticated_) advanceEntry(entryNeedsName_ ? EntryStep::Code : EntryStep::Loading);
        else enterPinKey(pinFocus_);
        return;
    case EntryStep::Code: advanceEntry(EntryStep::Name); break;
    case EntryStep::Name:
        if (nameIndex_ == int(NameCharacters.size())) {
            eraseNameCharacter();
        } else if (nameIndex_ == int(NameCharacters.size()) + 1) {
            if (draftName_.find_first_not_of(' ') == std::string::npos) return;
            playerName_ = draftName_;
            entryNeedsName_ = false;
            saveProfile();
            play(NameEnd, twoDxGain(83));
            advanceEntry(EntryStep::Loading);
        } else if (draftName_.size() < 8) {
            draftName_ += NameCharacters[nameIndex_];
            if (draftName_.size() == 8) nameIndex_ = int(NameCharacters.size()) + 1;
            audio_.sfx(NameDecide, twoDxGain(88), 1);
        }
        return;
    case EntryStep::CardRead:
    case EntryStep::Loading:
    case EntryStep::PaymentCheck:
    case EntryStep::ModeSelect: return;
    case EntryStep::Profile: advanceEntry(EntryStep::PaymentCheck); break;
    case EntryStep::PaymentNotice: advanceEntry(EntryStep::ModeSelect); return;
    }
    play(Decide);
}

void Cabinet::eraseNameCharacter() {
    if (draftName_.empty()) return;
    draftName_.pop_back();
    audio_.sfx(NameCancel, twoDxGain(58), 1);
}

void Cabinet::enterPinKey(int index) {
    if (entryStep_ != EntryStep::Pin || entryNext_ || pinVerification_ || entryAuthenticated_ || cardId_.empty()
        || now_ - entryAt_ < render_.afpDuration(entryFolder(), entryMovie(), "in")) return;
    pinFocus_ = index;
    pinError_.clear();
    audio_.sfx(PinInput, twoDxGain(78), 1);
    if (index == 11) {
        if (!pin_.empty()) pin_.pop_back();
        return;
    }
    pin_.append(PinKeys[index].substr(0, 4 - pin_.size()));
    if (pin_.size() < 4) return;
    if (pinPhase_ == PinPhase::Create) {
        pendingPin_ = pin_;
        pinPhase_ = PinPhase::Repeat;
    } else if (pinPhase_ == PinPhase::Repeat && pin_ != pendingPin_) {
        pendingPin_.clear();
        pinPhase_ = PinPhase::Create;
        pinError_ = "入力が一致しません。もう一度設定してください";
    } else {
        pinVerificationAt_ = now_;
        if (pinPhase_ == PinPhase::Verify) {
            audio_.startEffectLoop("entry_original/audio/s221_general_waiting_loop_02.wav", twoDxGain(94), 28620);
            navigator_.play(NavigatorCue::PinChecking, selectedCrew_, now_);
        }
        pinVerification_ = submitLocalPin(pin_);
    }
    pin_.clear();
}

bool Cabinet::setRoomFocus(int focus) {
    if (roomFocus_ == focus) return false;
    roomFocus_ = focus;
    return true;
}

void Cabinet::chooseCrew(int index) {
    if (crewPreview_ == index) return;
    crewPreview_ = index;
    keepCrewVisible();
    crewChangedAt_ = now_;
    play(Cursor);
}

int Cabinet::crewFamilyIndex() const {
    const auto& families = crewFamilies();
    const int id = crewInfo(crewPreview_).id;
    return int(std::find_if(families.begin(), families.end(),
                            [id](const auto& family) { return family.id == id; }) - families.begin());
}

void Cabinet::keepCrewVisible() {
    const int row = crewFamilyIndex() / 5 * 5;
    const int last = std::max(0, (int(crewFamilies().size()) - 1) / 5 * 5 - 20);
    crewOffset_ = std::clamp(crewOffset_, std::max(0, row - 20), std::min(row, last));
}

void Cabinet::cancelCrew() {
    if (roomTab_ == 2) {
        if (draftCard_ == appealCard_) return;
        draftCard_ = appealCard_;
        play(Decide);
        return;
    }
    if (roomTab_ == 3) {
        if (draftStyle_ == boardStyle_ && draftBase_ == boardBase_ && draftStickers_ == placedStickers_) return;
        draftStyle_ = boardStyle_;
        draftBase_ = boardBase_;
        draftStickers_ = placedStickers_;
        activeSticker_ = -1;
        stickerDragging_ = false;
        play(Decide);
        return;
    }
    if (crewPreview_ == selectedCrew_) return;
    play(Decide);
    crewButton_ = 0;
    crewButtonAt_ = now_;
    crewPreview_ = selectedCrew_;
    keepCrewVisible();
    crewChangedAt_ = now_;
}

void Cabinet::commitCustomization() {
    if (crewInfo(crewPreview_).installed) selectedCrew_ = options_.crew = crewPreview_;
    appealCard_ = draftCard_;
    boardStyle_ = draftStyle_;
    boardBase_ = draftBase_;
    saveProfile();
}

bool Cabinet::stickerFits(const PlacedSticker& sticker, const std::vector<PlacedSticker>& placed, int ignored) const {
    const auto& art = boardStickers_[sticker.index];
    if (sticker.column < 0 || sticker.row < 0 || sticker.column > StickerColumns - art.columns
        || sticker.row > StickerRows - art.rows) return false;
    for (int i = 0; i < int(placed.size()); ++i) {
        const auto& other = placed[i];
        const auto& otherArt = boardStickers_[other.index];
        if (i != ignored && sticker.column < other.column + otherArt.columns && other.column < sticker.column + art.columns
            && sticker.row < other.row + otherArt.rows && other.row < sticker.row + art.rows) return false;
    }
    return true;
}

void Cabinet::placeSticker(int index) {
    stickerFocus_ = index;
    const auto& art = boardStickers_[index];
    for (int row = 0; row <= StickerRows - art.rows; ++row) {
        for (int column = 0; column <= StickerColumns - art.columns; ++column) {
            const PlacedSticker placement{index, column, row};
            if (!stickerFits(placement, draftStickers_)) continue;
            activeSticker_ = int(draftStickers_.size());
            draftStickers_.push_back(placement);
            play(Cursor);
            return;
        }
    }
}

void Cabinet::removeSticker() {
    if (activeSticker_ < 0) return;
    draftStickers_.erase(draftStickers_.begin() + activeSticker_);
    activeSticker_ = int(draftStickers_.size()) - 1;
    stickerDragging_ = false;
    play(Decide);
}

void Cabinet::updateAchievements() {
    constexpr std::array<std::string_view, 10> grades{"s","aaa_plus","aaa","aa_plus","aa","a_plus","a","b","c","d"};
    constexpr std::array achievementLampRank{0, 1, 2, 2, 2, 3, 4, 5};
    for (auto& achievement : achievements_) {
        const auto& metric = achievement.metric;
        int progress = 0;
        if (metric == "plays") progress = playCount_;
        else if (metric == "today") progress = todayPlays_;
        else if (metric == "days") progress = playDays_;
        else if (metric == "day_chain") progress = dayChain_;
        else if (metric == "week_chain") progress = weekChain_;
        else if (metric == "parts") progress = int(boardBases_.size() + boardStickers_.size());
        else if (metric == "lamp" || metric == "grade") {
            for (const auto& [key, record] : records_) {
                if (metric == "lamp") progress += achievementLampRank[lampRank(record.outcome)] >= achievement.parameter;
                else progress += std::find(grades.begin(), grades.end(), record.grade) - grades.begin() <= achievement.parameter;
            }
        } else if (metric == "level") {
            for (const auto& song : songs_) for (const auto& chart : song.charts)
                if (chart.track.level / 10 == achievement.parameter && lampRank(record(chart.track).outcome) >= 2) ++progress;
        }
        achievement.progress = std::min(progress, achievement.goal);
    }
}

bool Cabinet::achievementReady(const Achievement& achievement) const {
    return achievement.progress >= achievement.goal
        && std::find(claimedAchievements_.begin(), claimedAchievements_.end(), achievement.id) == claimedAchievements_.end()
        && (!achievement.prerequisite || std::find(claimedAchievements_.begin(), claimedAchievements_.end(), achievement.prerequisite) != claimedAchievements_.end());
}

void Cabinet::claimAchievement(int index) {
    std::vector<int> claimed;
    int packets = 0, blocks = 0, titles = 0;
    for (int row = 0; row < int(achievements_.size()); ++row) {
        auto& achievement = achievements_[row];
        if ((index < 0 ? achievement.category != achievementCategory_ : row != index)
            || !achievementReady(achievement)) continue;
        claimed.push_back(achievement.id);
        if (achievement.rewardType == 0) { blocks_ += achievement.reward; blocks += achievement.reward; }
        else if (achievement.rewardType == 2) { packets_ += achievement.reward; packets += achievement.reward; }
        else if (achievement.rewardType == 1) { earnedTitles_.push_back(achievement.reward); ++titles; }
    }
    if (claimed.empty()) return;
    claimedAchievements_.insert(claimedAchievements_.end(), claimed.begin(), claimed.end());
    saveProfile();
    achievementPopupAt_ = now_;
    achievementRewardText_.clear();
    for(const auto& reward:std::array<std::pair<int,std::string_view>,3>{{{packets,"PACKET"},{blocks,"BLOCK"},{titles,"アピール称号"}}}) {
        if (!reward.first) continue;
        if (!achievementRewardText_.empty()) achievementRewardText_+=" / ";
        achievementRewardText_+=std::to_string(reward.first)+" "+std::string(reward.second);
    }
    achievementRewardText_ += " をGETしました";
    play(index < 0 ? "menu/audio/se_01_short004_achievement_all.wav"
                   : "menu/audio/se_01_short003_achievement.wav");
}

void Cabinet::toggleWheel(Wheel wheel) {
    if (songConfirm_ || songOverlayOutAt_>=0) return;
    songPromptPending_ = false;
    if (wheel==Wheel::Sort) {
        if (sortOpen_) { closeSongOverlay(true); return; }
        sortOpen_=true; sortFocus_=sort_; songOverlayAt_=now_; songOverlayOutAt_=-1;
        resetInput(); play(EntryOpenLow);
    } else {
        if (sortOpen_) return;
        if (wheel_==Wheel::Songs) { wheel_=Wheel::Folder; wheelIndex_=std::max(0,activeFolder_); audio_.stopPreview(true); previewAt_=-1; }
        else if (activeFolder_>=0) { wheel_=Wheel::Songs; previewAt_=now_+SongPreviewDelay; }
        play(Cursor);
    }
}

void Cabinet::closeSongOverlay(bool accept) {
    if (songOverlayOutAt_>=0) return;
    if (sortOpen_) {
        if (accept && sort_!=sortFocus_) { sort_=sortFocus_; if (activeFolder_>=0) rebuildSongs(); play(Decide); }
        play(EntryCloseLow);
    } else {
        if (accept) {
            if (!chart().installed) {
                notice("楽曲データがインストールされていません"); return;
            }
            const auto decide=chart().track.presentation("decide.wav");
            play(chart().track.id != 666 && hasAsset(decide) ? std::string_view(decide) : Decide);
        }
        songConfirmAccept_=accept;
        play(DifficultyClose);
    }
    songOverlayOutAt_=now_; resetInput();
}

void Cabinet::openSettings() {
    songPromptPending_ = false;
    settingsReturn_ = page_;
    coveredAt_ = entered_;
    page_ = Page::Settings;
    entered_ = now_;
    optionFocus_ = 0;
    optionPopup_ = -1;
    settingsLeavingAt_ = -1;
    nextSettingsTab_.reset();
    playTab_ = settingsReturn_ == Page::Room ? 1 : 0;
    settingsTab_ = SettingsTab::General;
    bindTarget_ = -1;
    searchEditing_ = false;
    resetInput();
    play("menu/audio/cursor/se_04_cursor004_submonitor1.wav");
}

double Cabinet::settingsDuration(std::string_view clip) const {
    if (playTab_ != 1 || settingsTab_ == SettingsTab::Controller) return 0;
    return render_.afpDuration("settings", "page_option", clip);
}

void Cabinet::switchSettingsTab(SettingsTab tab) {
    optionPopup_ = -1;
    bindTarget_ = -1;
    settingsLeavingAt_ = now_;
    nextSettingsTab_ = tab;
    resetInput();
    play("menu/audio/cursor/se_04_cursor004_submonitor1.wav");
}

void Cabinet::activateBinding(int control) {
    bindingFocus_ = control;
    bindTarget_ = control < ButtonCount ? control : -1;
    if (bindTarget_ >= 0) return;
    const bool joystick = controllerMode_ == ControllerMode::Joystick;
    auto& keys = joystick ? controllerMap_ : keyMap_;
    auto& swapped = joystick ? controllerLeftUsesY_ : leftUsesX_;
    auto& reverse = joystick ? controllerReverse_ : reverse_;
    switch (control) {
    case Axes: swapped = !swapped; break;
    case ReverseLeft: case ReverseRight: reverse[control - ReverseLeft] = !reverse[control - ReverseLeft]; break;
    case ResetBindings:
        keys = joystick ? DefaultControllerKeys : DefaultKeys;
        swapped = false;
        reverse.fill(false);
        break;
    }
    calibration_.fill(0);
    resetInput();
    saveProfile();
}

void Cabinet::searchSongs() {
    searchMatches_.clear();
    const auto query = normalizeSearch(searchQuery_);
    if (!query.empty()) {
        for (int index = 0; index < int(songs_.size()); ++index)
            if (chartRange(songs_[index], false).second > 0 &&
                songs_[index].searchKey.find(query) != std::string::npos) searchMatches_.push_back(index);
    }
    searchFocus_ = 0;
}

void Cabinet::rebuildCards() {
    visibleCards_.clear();
    const int genres = cardFilter_ & CardGenreMask;
    const int series = cardFilter_ & CardSeriesMask;
    for (int i = 0; i < int(appealCards_.size()); ++i) {
        const auto& card = appealCards_[i];
        if ((genres && !(card.filterMask & genres)) || (series && !(card.filterMask & series))) continue;
        if (favoriteCardsOnly_ && std::find(favoriteCards_.begin(), favoriteCards_.end(), card.id) == favoriteCards_.end()) continue;
        visibleCards_.push_back(i);
    }
    const auto selected = std::find(visibleCards_.begin(), visibleCards_.end(), draftCard_);
    const int lastPage = std::max(0, (int(visibleCards_.size()) - 1) / 5 * 5 - 15);
    cardOffset_ = selected == visibleCards_.end() ? 0 : std::min(int(selected - visibleCards_.begin()) / 20 * 20, lastPage);
}

std::string_view Cabinet::roomPageName() const {
    return std::array{"myroom_home", "myroom_crew", "myroom_apica", "myroom_plate", "page_achievement"}[roomTab_];
}

void Cabinet::switchRoomTab(int tab) {
    if (roomTab_ == tab) return;
    const int previousTab = roomTab_;
    cardFilterOpen_ = false;
    if (selectedCrew_ != crewPreview_ && crewInfo(crewPreview_).installed) {
        selectedCrew_ = options_.crew = crewPreview_;
        saveProfile();
    }
    roomTab_ = tab;
    if (tab == 1) navigator_.play(NavigatorCue::CrewSelect, selectedCrew_, now_);
    else if (tab == 2) navigator_.play(NavigatorCue::AppealCardSelect, selectedCrew_, now_);
    else if (previousTab == 1 || previousTab == 2) navigator_.play(NavigatorCue::Silent, selectedCrew_, now_);
    entered_ = now_;
    resetInput();
    play("menu/audio/cursor/se_04_cursor004_submonitor1.wav");
}

void Cabinet::startResultForce() {
    forceAt_ = now_;
    play(forceEffect_ == 3 ? "result_force/audio/class.wav" : "result_force/audio/plus.wav");
    navigator_.play(forceEffect_ == 3 ? NavigatorCue::ForceClassUp
        : forceEffect_ == 2 ? NavigatorCue::ForceLevelUp : NavigatorCue::ForceIncrease, selectedCrew_, now_);
}

void Cabinet::confirm() {
    if (page_==Page::Songs && (sortOpen_ || songConfirm_)) {
        if (now_-songOverlayAt_>=render_.afpDuration("selection_ui",sortOpen_?"sort":"window_select","in")) closeSongOverlay(true);
        return;
    }
    if (page_ == Page::Credit) {
        if (paymentAcceptedAt_ >= 0) {
            // 180253a80 accepts START after 1000 ms in the receipt state.
            if (paymentReceiptAt_ >= 0 && paymentReceiptOutAt_ < 0
                && now_ - paymentReceiptAt_ >= PaymentReceiptSkipTime) {
                paymentReceiptOutAt_ = now_;
                play(EntryCloseLow);
            }
            return;
        }
        if (now_ - entered_ < render_.afpDuration("payment", "paseli_credit", "in")) return;
    }
    if (page_ == Page::Mode && now_ - modeStarted_ < render_.afpDuration("mode_flow", "mode_select", "in")) return;
    if (premiumExit() && now_ - entered_ < render_.afpDuration("session_dialog", "win_yes_no", "in")) return;
    if (page_ == Page::Caution && now_ - entered_ < render_.afpDuration("caution_original","caution","0")) return;
    if (page_ == Page::Stage || transition_ || roomDepartureAt_ >= 0) return;
    if (cardFilterOpen_) {
        cardFilter_ = draftCardFilter_;
        rebuildCards();
        cardFilterOpen_ = false;
        play(Decide);
        return;
    }
    if (page_ == Page::Crew && roomTab_ == 1
        && (crewPreview_ == selectedCrew_ || !crewInfo(crewPreview_).installed)) return;
    if (page_ == Page::Crew && roomTab_ == 2 && draftCard_ == appealCard_) return;
    if (page_ == Page::Crew && roomTab_ == 3 && draftStyle_ == boardStyle_ && draftBase_ == boardBase_ && draftStickers_ == placedStickers_) return;
    if (page_ != Page::Pin && page_ != Page::Credit && page_ != Page::Matching && page_ != Page::Caution
        && page_ != Page::Saved && page_ != Page::GameEnd
        && page_ != Page::Songs)
        play(page_ == Page::Room && RoomPages[roomFocus_] == Page::Caution ? "room_flow/se_05_decide003_high.wav" : Decide);
    switch (page_) {
    case Page::Title: go(Page::Pin); break;
    case Page::Pin: confirmEntry(); break;
    case Page::Room:
        if (RoomPages[roomFocus_] == Page::Caution) {
            roomDepartureAt_ = now_;
            navigator_.play(NavigatorCue::Depart, selectedCrew_, now_);
        } else if (RoomPages[roomFocus_] == Page::Crew) enter(Page::Crew);
        else go(RoomPages[roomFocus_]);
        break;
    case Page::Generator:
        if (generatorPhase_ >= 2) break;
        if (generatorPhase_ == 0) {
            if (packets_ < GeneratorPrice * generatorCount_) { notice("PACKET が不足しています"); break; }
            generatorPhase_ = 1;
            generatorFocus_ = 1;
            generatorAt_ = now_;
        } else if (generatorFocus_ == 1) generatorPhase_ = 0;
        else {
            // APIGENE7 volume 1: 26 bases + 136 stickers, 100 PACKET.
            // Missing source artwork is a failed request, never a charged blank reward.
            generatorPhase_ = 0;
            std::vector<int> drawn;
            generatorRewards_.clear();
            for (int i = 0; i < generatorCount_; ++i) {
                const int item = SDL_rand(162);
                const bool base = item < 26;
                const int id = base ? item + 1 : item - 25;
                const auto& catalog = base ? boardBases_ : boardStickers_;
                const auto found = std::find_if(catalog.begin(), catalog.end(), [=](const auto& row) { return row.id == id; });
                if (found == catalog.end()) break;
                generatorRewards_.emplace_back(base, found->image);
                drawn.push_back((base ? 23 : 24) * 10000 + id);
            }
            if (int(drawn.size()) != generatorCount_) {
                notice("アイテムの画像データがありません");
                break;
            }
            packets_ -= GeneratorPrice * generatorCount_;
            for (const int key : drawn)
                if (std::find(generatedParts_.begin(), generatedParts_.end(), key) == generatedParts_.end())
                    generatedParts_.push_back(key);
            generatorRewardIndex_ = 0;
            generatorPhase_ = 2;
            generatorAt_ = now_;
            saveProfile();
            play("generator_vii/roll.wav");
        }
        break;
    case Page::Valkyrie: notice("チケットがありません"); break;
    case Page::Crew:
        if (roomTab_ == 0) { startAfterSubscreen_ = true; back(); break; }
        if (roomTab_ == 2) {
            appealCard_ = draftCard_;
            saveProfile();
            navigator_.play(NavigatorCue::AppealCardSaved, selectedCrew_, now_);
            break;
        }
        if (roomTab_ == 3) {
            boardStyle_ = draftStyle_;
            boardBase_ = draftBase_;
            placedStickers_ = draftStickers_;
            saveProfile();
            break;
        }
        if (roomTab_ != 1) break;
        crewButton_ = 1;
        crewButtonAt_ = now_;
        selectedCrew_ = options_.crew = crewPreview_;
        saveProfile();
        break;
    case Page::Caution:
        startTrackSelection();
        break;
    case Page::Mode:
        if (!modePayments(modeIndex_).empty()) enter(Page::Credit);
        else notice("このモードは現在利用できません");
        break;
    case Page::Credit: {
        const auto offers = modePayments(modeIndex_);
        if (creditFocus_ == static_cast<int>(offers.size())) { paymentCanceled_ = true; paymentAcceptedAt_ = now_; play(EntryCloseLow); play(Decide); }
        else startSession(offers[creditFocus_]);
        break;
    }
    case Page::Skill: {
        const double intro = render_.afpDuration("skill_effects", "in_skill_sel", "in");
        if (now_ - entered_ < intro) break;
        const auto& course = courses_[courseIndex_];
        for (const auto& [songIndex, chartIndex] : course.tracks) {
            if (!songs_[songIndex].charts[chartIndex].installed) {
                notice("コースの楽曲データがインストールされていません");
                return;
            }
        }
        activeCourse_ = courseIndex_;
        play(Decide);
        go(Page::Matching);
        break;
    }
    case Page::Songs:
        songPromptPending_ = false;
        if (wheel_==Wheel::Songs) {
            songConfirm_=true; songConfirmAccept_=false; songOverlayAt_=now_; songOverlayOutAt_=-1;
            resetInput(); play(DifficultyOpen);
        } else {
            const auto folder=songFolders_[wheelIndex_];
            play(Decide);
            if (folder.expandable) {
                activeFolder_ = -1;
                if (folderGroup_ == folder.kind && folderGroupValue_ == folder.value) rebuildFolders();
                else rebuildFolders(folder.kind, folder.value);
            } else if (activeFolder_==wheelIndex_) { activeFolder_=-1; }
            else {
                if (!folder.child && folderGroup_ != FolderKind::All) rebuildFolders();
                activeFolder_=wheelIndex_;
                wheel_=Wheel::Songs; rebuildSongs();
            }
        }
        break;
    case Page::Matching:
        if (practiceExpired()) {
            go(sessionResults_.empty() ? Page::Saved : Page::Summary);
        } else if (!matchingGo_) {
            matchingGo_ = true;
            stageOptions_ = options_;
            stageOptions_.randomSeed = SDL_rand_bits();
            stageOptions_.trackNumber = track_;
            stageOptions_.practiceUntil = practiceUntil_;
            stageOptions_.autoRetry &= sessionType_ == StartType::Premium;
            if (activeCourse_) {
                stageOptions_.courseTracks = static_cast<int>(courses_[*activeCourse_].tracks.size());
                // 1805f65f0 supplies the previous result's unrounded rate to 1804b3750.
                stageOptions_.permissiveRate = track_ == 1 ? 1 : sessionResults_.back().result.gauge;
                stageOptions_.excessive = stageOptions_.ultra = stageOptions_.ars =
                    stageOptions_.sCritical = stageOptions_.autoPlay = false;
            }
            if (sessionType_ == StartType::Light) {
                stageOptions_.ultra = stageOptions_.ars = stageOptions_.sCritical = stageOptions_.exScore = false;
            }
            play("flow/se_01_short002_matching_ok.wav", .9f);
            go(Page::Stage);
        }
        break;
    case Page::Result:
        if (resultVoicePending()) {
            if (navigator_.celebratingResult()) {
                if (!forceDone_) startResultForce();
                else navigator_.play(NavigatorCue::Silent, selectedCrew_, now_);
            }
            return;
        }
        if (!forceDone_) {
            if (forceAt_ < 0) {
                startResultForce();
                return;
            }
            forceDone_ = true;
            navigator_.play(NavigatorCue::Silent, selectedCrew_, now_);
            return;
        }
        if (activeCourse_) {
            if (lower(result_.outcome) == "crash" || track_ == static_cast<int>(courses_[*activeCourse_].tracks.size())) {
                go(Page::Summary);
            } else {
                ++track_;
                go(Page::Matching);
            }
        // 180354dc0 checks the last result after the normal track quota.
        } else if (sessionType_ == StartType::Standard && track_ == 3 &&
                   !result_.autoPlay && lampRank(result_.outcome) >= lampRank("comp")) {
            ++track_;
            go(Page::Songs, true);
        } else if (practiceExpired() || (sessionType_ != StartType::Premium && track_ >= 3)
            || (sessionType_ == StartType::Light && lower(result_.outcome) == "crash")) go(Page::Summary);
        else {
            ++track_;
            go(Page::Songs, true);
        }
        break;
    case Page::Summary:
        if (skillTitleAt_ >= 0) {
            if (skillTitleDecidedAt_ >= 0 ||
                now_ - skillTitleAt_ < render_.afpDuration("skill_effects/windows", "win_skill_change", "in")) return;
            skillTitleDecidedAt_ = now_;
            if (skillTitleFocus_ == 0) {
                skillTitle_ = courses_[*activeCourse_].nameId;
                saveProfile();
            }
            play("skill_effects/audio/title-decide.wav");
            return;
        }
        if (activeCourse_) {
            const auto changeEnd = skillLevelChanged_ ? SkillChangeAt + render_.afpDuration("skill_effects", "skill_change", "in") : 0;
            if (now_ - entered_ < skillResultIntroDuration() + changeEnd) return;
            if (skillPassed()) {
                if (skillBonusPhase_ == SkillBonusPhase::Waiting) {
                    if (now_ - skillBonusAt_ < render_.afpDuration("skill_effects/windows", "win_skill_bonus", "in")) return;
                    const auto& course = courses_[*activeCourse_];
                    clearedSkillCourses_.emplace_back(course.seasonId, course.id);
                    skillBonusPackets_ = std::min(skillBonus_, 99999999 - packets_);
                    skillBonusBlocks_ = std::min(skillBonus_, 99999999 - blocks_);
                    packets_ += skillBonusPackets_;
                    blocks_ += skillBonusBlocks_;
                    sessionPackets_ += skillBonusPackets_;
                    sessionBlocks_ += skillBonusBlocks_;
                    saveProfile();
                    audio_.startEffectLoop(CountMove, twoDxGain(48));
                    skillBonusPhase_ = SkillBonusPhase::Counting;
                    skillBonusAt_ = now_;
                    return;
                }
                if (skillBonusPhase_ == SkillBonusPhase::Ready) {
                    skillBonusPhase_ = SkillBonusPhase::Closing;
                    skillBonusAt_ = now_;
                    play("skill_effects/audio/bonus-out.wav");
                    return;
                }
                if (skillBonusPhase_ != SkillBonusPhase::Done) return;
            }
        }
        if (!summaryCountDone_) {
            finishSummaryCount();
            return;
        }
        practiceUntil_ = 0;
        continuing_ = true;
        exitFocus_ = 1;
        go(Page::Exit);
        break;
    case Page::Exit:
        exitDecidedAt_ = now_;
        if (continuing_) navigator_.play(exitFocus_ == 0 ? NavigatorCue::ContinueYes : NavigatorCue::ContinueNo,
                                         selectedCrew_, now_);
        else if (premiumExit()) play(EntryCloseLow);
        break;
    case Page::Saved:
        if (now_ - entered_ >= 4200) go(Page::GameEnd);
        break;
    case Page::GameEnd:
        if (now_ - entered_ >= 6500) go(Page::Title);
        break;
    case Page::Settings:
        if (playTab_ == 0) {
            playTab_ = 1;
            entered_ = now_;
            play("menu/audio/cursor/se_04_cursor004_submonitor1.wav");
            break;
        }
        if (controllerSettings()) {
            if (bindingFocus_ < BindingControlCount) activateBinding(bindingFocus_);
            else switchSettingsTab(SettingsTab::General);
            break;
        }
        if (playTab_ == 1 && optionFocus_ == ControllerTabFocus) {
            switchSettingsTab(SettingsTab::Controller);
            break;
        }
        if (playTab_ == 2) {
            searchEditing_ = false;
            searchComposition_.clear();
            if (searchMatches_.empty() || activeCourse_) break;
            const int selected = searchMatches_[searchFocus_];
            if (settingsReturn_ == Page::Songs && selected != visibleSongs_[songIndex_]) audio_.stopPreview();
            rebuildFolders();
            activeFolder_ = folderIndex(FolderKind::All);
            wheel_ = Wheel::Songs;
            rebuildSongs();
            songIndex_ = int(std::ranges::find(visibleSongs_, selected) - visibleSongs_.begin());
            chartIndex_ = displayChart(song());
            // A new song chosen from Matching must pass the normal difficulty confirmation.
            if (settingsReturn_ == Page::Matching) settingsReturn_ = Page::Songs;
            back();
            break;
        }
        if (playTab_ == 1 && optionPopup_ < 0 &&
            (optionFocus_ == static_cast<int>(Option::Judgment) || optionFocus_ == static_cast<int>(Option::Display))) {
            options_.toggleTimingPrecision(optionFocus_ == static_cast<int>(Option::Display));
            play(Decide);
            break;
        }
        if (playTab_ == 1 && optionPopup_ < 0 &&
            (optionFocus_ == static_cast<int>(Option::LanePreview) || optionFocus_ == static_cast<int>(Option::Ars))) {
            const auto option = static_cast<Option>(optionFocus_);
            adjust(option, 1 - 2 * optionValue(option));
            break;
        }
        if (optionPopup_ >= 0) optionPopup_ = -1;
        else back();
        break;
    case Page::Stage: break;
    }
}

void Cabinet::back() {
    if (page_ == Page::Songs) songPromptPending_ = false;
    if (page_==Page::Songs && (sortOpen_ || songConfirm_)) { closeSongOverlay(false); return; }
    if (cardFilterOpen_) { cardFilterOpen_ = false; return; }
    switch (page_) {
    case Page::Title: case Page::Stage: case Page::Saved: case Page::GameEnd: return;
    case Page::Pin:
        if (entryNext_) break;
        if (pinVerification_) {
            pinVerification_.reset();
            if (pinPhase_ == PinPhase::Verify) audio_.stopEffectLoop();
            go(Page::Title);
            break;
        }
        if (entryStep_ == EntryStep::Transfer || entryStep_ == EntryStep::Terms) advanceEntry(EntryStep::Registration);
        else if (entryStep_ == EntryStep::Name) eraseNameCharacter();
        else if (!entryAuthenticated_) go(Page::Title);
        break;
    case Page::Crew:
        commitCustomization();
        crewSwitchPending_ = selectedCrew_ != crewOnMain_;
        if (crewSwitchPending_) navigator_.play(NavigatorCue::Hidden, selectedCrew_, now_);
        else if (roomTab_ == 1 || roomTab_ == 2) navigator_.play(NavigatorCue::Silent, selectedCrew_, now_);
        crewLeavingAt_ = now_;
        break;
    case Page::Generator:
        if (generatorPhase_ >= 2) break;
        if (generatorPhase_ == 1) generatorPhase_ = 0;
        else go(Page::Room);
        break;
    case Page::Valkyrie: case Page::Skill: go(Page::Room); break;
    case Page::Caution: go(Page::Room); break;
    case Page::Mode: go(Page::Title); break;
    case Page::Credit:
        if (paymentAcceptedAt_ < 0) {
            paymentCanceled_ = true;
            paymentAcceptedAt_ = now_;
            play(EntryCloseLow);
        }
        break;
    case Page::Room: case Page::Songs:
        if (page_==Page::Songs && activeFolder_ >= 0) {
            wheel_=Wheel::Folder; wheelIndex_=std::max(0,activeFolder_); activeFolder_=-1;
            audio_.stopPreview(true); previewAt_=-1;
        } else if (page_==Page::Songs && folderGroup_!=FolderKind::All) {
            wheelIndex_ = folderIndex(folderGroup_, folderGroupValue_);
            activeFolder_ = -1;
            rebuildFolders();
        }
        else {
            continuing_ = false;
            exitReturn_ = page_;
            exitElapsed_ = now_ - entered_;
            exitFocus_ = 1;
            enter(Page::Exit);
        }
        break;
    case Page::Matching:
        if (!activeCourse_) go(Page::Songs);
        else if (track_ == 1) go(Page::Skill);
        break;
    case Page::Result: confirm(); break;
    case Page::Summary:
        if (skillTitleAt_ < 0 && (skillBonusPhase_ == SkillBonusPhase::Pending ||
                                  skillBonusPhase_ == SkillBonusPhase::Done)) confirm();
        break;
    case Page::Exit: exitFocus_ = 1; confirm(); break;
    case Page::Settings:
        if (bindTarget_ >= 0) { bindTarget_ = -1; break; }
        if (optionPopup_ >= 0) { optionPopup_ = -1; break; }
        saveProfile();
        settingsLeavingAt_ = now_;
        play(Decide);
        resetInput();
        break;
    }
}

void Cabinet::rotate(int side, int steps) {
    if (!steps) return;
    if (page_ == Page::Songs) songPromptPending_ = false;
    if (page_==Page::Songs && songOverlayOutAt_>=0) return;
    if (page_==Page::Songs && sortOpen_) {
        if (side != 1) return;
        const int next=std::clamp(sortFocus_+steps,0,int(SongSorts.size())-1);
        if (next!=sortFocus_) { sortFocus_=next; play("menu/audio/cursor/se_04_cursor002_musicselect.wav"); }
        return;
    }
    if (page_==Page::Songs && songConfirm_) {
        const auto [begin, end] = chartRange(song(), exChallengeFolder());
        const int next=std::clamp(chartIndex_+steps,begin,end-1);
        if (next!=chartIndex_) { chartIndex_=next; play("menu/audio/cursor/se_04_cursor003_difficulty.wav"); }
        return;
    }
    if (cardFilterOpen_) return;
    if ((page_ == Page::Crew && roomTab_ == 4) || (page_ == Page::Settings && playTab_ == 3)) {
        if (side == 0) {
            const int current = int(std::find(AchievementFolders.begin(), AchievementFolders.end(), achievementCategory_) - AchievementFolders.begin());
            const int index = wrap(current + steps, int(AchievementFolders.size()));
            achievementCategory_ = AchievementFolders[index];
            achievementCategoryOffset_ = std::clamp(index-3,0,int(AchievementFolders.size())-8);
            achievementOffset_ = 0;
        } else {
            const int count = int(std::count_if(achievements_.begin(), achievements_.end(), [&](const auto& a) { return a.category == achievementCategory_; }));
            achievementOffset_ = std::clamp(achievementOffset_ + steps, 0, std::max(0,count-8));
        }
        play(Cursor);
        return;
    }
    switch (page_) {
    case Page::Pin:
        if (entryNext_ || pinVerification_) return;
        if (entryStep_ == EntryStep::Name) {
            const int next = wrap(nameIndex_ + steps, int(NameCharacters.size()) + 2);
            if (next != nameIndex_) {
                nameIndex_ = next;
                audio_.sfx(NameCursor, twoDxGain(55), 1);
            }
            return;
        }
        else if (entryStep_ == EntryStep::Registration || entryStep_ == EntryStep::Terms) entryFocus_ = wrap(entryFocus_ + steps, 2);
        else if (entryStep_ == EntryStep::Pin) pinFocus_ = wrap(pinFocus_ + steps * (side == 0 ? 1 : 3), PinKeys.size());
        else return;
        break;
    case Page::Exit: exitFocus_ = wrap(exitFocus_ + steps, 2); break;
    case Page::Summary:
        if (skillTitleAt_ >= 0 && skillTitleDecidedAt_ < 0 &&
            now_ - skillTitleAt_ >= render_.afpDuration("skill_effects/windows", "win_skill_change", "in")) {
            const int focus = steps < 0 ? 0 : 1;
            if (focus != skillTitleFocus_) {
                skillTitleFocus_ = focus;
                skillTitleFocusAt_ = now_;
                play(SkillTitleCursor);
            }
        }
        return;
    case Page::Generator:
        if (generatorPhase_ == 0 && side == 0) {
            const int limit = std::clamp(packets_ / GeneratorPrice, 1, GeneratorBatchLimit);
            const int next = std::clamp(generatorCount_ + steps, 1, limit);
            if (next == generatorCount_) return;
            generatorCount_ = next;
        }
        else if (generatorPhase_ == 1 && side == 1) generatorFocus_ = wrap(generatorFocus_ + steps, 2);
        else return;
        break;
    case Page::Valkyrie: valkyrieVolume_ = wrap(valkyrieVolume_ - 1 + steps, 22) + 1; break;
    case Page::Room:
        if (!setRoomFocus(wrap(roomFocus_ + steps, static_cast<int>(RoomPages.size())))) return;
        break;
    case Page::Crew:
        if (roomTab_ == 1) {
            const auto& families = crewFamilies();
            const int family = crewFamilyIndex();
            if (side == 0 && families[family].styles.size() > 1) {
                const auto& styles = families[family].styles;
                const int current = int(std::find(styles.begin(), styles.end(), crewPreview_) - styles.begin());
                chooseCrew(styles[wrap(current + steps, int(styles.size()))]);
            } else {
                chooseCrew(families[wrap(family + steps, int(families.size()))].styles.front());
            }
        }
        else if (roomTab_ == 2 && !visibleCards_.empty()) {
            const auto current = std::find(visibleCards_.begin(), visibleCards_.end(), draftCard_);
            const int start = current != visibleCards_.end() ? int(current - visibleCards_.begin()) : steps > 0 ? -1 : 0;
            const int index = wrap(start + steps, int(visibleCards_.size()));
            draftCard_ = visibleCards_[index];
            cardOffset_ = index / 20 * 20;
            play(Cursor);
        } else if (roomTab_ == 3) {
            if (decorationTab_ == 0) draftStyle_ = wrap(draftStyle_ + steps, 2);
            else if (decorationTab_ == 1) {
                draftBase_ = wrap(draftBase_ + steps, int(boardBases_.size()));
                decorationOffset_ = draftBase_ / 2 * 2;
            } else {
                stickerFocus_ = wrap(stickerFocus_ + steps, int(boardStickers_.size()));
                decorationOffset_ = std::min(stickerFocus_ / 20 * 20, (int(boardStickers_.size()) - 1) / 5 * 5 - 15);
            }
            play(Cursor);
        }
        return;
    case Page::Mode: modeIndex_ = wrap(modeIndex_ + steps, ModeCount); break;
    case Page::Skill: {
        if (now_ - entered_ < render_.afpDuration("skill_effects", "in_skill_sel", "in")) return;
        const int previous = courseIndex_;
        const auto& selected = courses_[courseIndex_];
        if (side == 0) {
            std::vector<int> levels;
            for (const auto& course : courses_)
                if (std::find(levels.begin(), levels.end(), course.level) == levels.end()) levels.push_back(course.level);
            const auto position = std::find(levels.begin(), levels.end(), selected.level) - levels.begin();
            const int level = levels[wrap(static_cast<int>(position) + steps, static_cast<int>(levels.size()))];
            auto next = std::find_if(courses_.begin(), courses_.end(), [&](const auto& course) {
                return course.level == level && course.set == selected.set;
            });
            if (next == courses_.end())
                next = std::find_if(courses_.begin(), courses_.end(), [=](const auto& course) { return course.level == level; });
            courseIndex_ = static_cast<int>(next - courses_.begin());
        } else {
            std::vector<int> choices;
            for (int i = 0; i < static_cast<int>(courses_.size()); ++i)
                if (courses_[i].level == selected.level) choices.push_back(i);
            const auto position = std::find(choices.begin(), choices.end(), courseIndex_) - choices.begin();
            courseIndex_ = choices[wrap(static_cast<int>(position) + steps, static_cast<int>(choices.size()))];
        }
        if (courseIndex_ != previous) {
            if (side == 0)
                audio_.sfx("skill_effects/audio/s103_music_select_level_cursor.wav", twoDxGain(78), 1);
            else
                play(Cursor);
        }
        return;
    }
    case Page::Credit:
        if (paymentAcceptedAt_ >= 0) return;
        creditFocus_ = std::clamp(creditFocus_ + steps, 0, static_cast<int>(modePayments(modeIndex_).size()));
        break;
    case Page::Songs:
        if (side==0 && wheel_==Wheel::Songs) {
            if (visibleCharts_[songIndex_] < 0) {
                const auto [begin, end] = chartRange(song(), exChallengeFolder());
                chartIndex_ = std::clamp(chartIndex_ + steps, begin, end - 1);
                if (!exChallengeFolder()) {
                    preferredChart_ = chartIndex_;
                    if (sort_ == 3 || sort_ == 5 || sort_ == 6) rebuildSongs();
                }
            }
        } else {
            if (held_[FXL] || held_[FXR]) { steps=steps>0?3:-3; fxCombined_=true; }
            moveSongList(steps);
        }
        break;
    case Page::Matching:
        if (held_[Start]) {
            startUsed_ = true;
            adjust(Option::Speed, steps);
        }
        break;
    case Page::Settings:
        if (playTab_ == 2) {
            if (!searchMatches_.empty()) searchFocus_ = std::clamp(searchFocus_ + steps, 0, int(searchMatches_.size()) - 1);
            play("menu/audio/cursor/se_04_cursor005_submonitor2.wav");
            return;
        }
        if (playTab_ != 1) return;
        if (settingsTab_ == SettingsTab::Controller) {
            if (side == 0 && bindTarget_ < 0)
                bindingFocus_ = wrap(bindingFocus_ + steps, BindingControlCount + 1);
            else return;
            break;
        }
        if (side == 0) {
            optionFocus_ = wrap(optionFocus_ + steps, ControllerTabFocus + 1);
            optionPopup_ = -1;
        } else if (optionFocus_ != ControllerTabFocus) adjust(static_cast<Option>(optionFocus_), steps);
        else return;
        break;
    default: return;
    }
    play(page_ == Page::Mode || page_ == Page::Credit ? "menu/audio/cursor/se_04_cursor001_modeselect.wav"
        : page_ == Page::Songs ? (side == 0 && wheel_ == Wheel::Songs
            ? "menu/audio/cursor/se_04_cursor003_difficulty.wav" : "menu/audio/cursor/se_04_cursor002_musicselect.wav")
        : page_ == Page::Settings ? "menu/audio/cursor/se_04_cursor005_submonitor2.wav" : Cursor);
}

int Cabinet::optionValue(Option option) const {
    switch (option) {
    case Option::Speed: return static_cast<int>(std::lround(options_.speed * 100));
    case Option::Judgment: return options_.timingValue(0);
    case Option::Display: return options_.timingValue(1);
    case Option::Brightness: return options_.brightness;
    case Option::Timing: return options_.cueStyle;
    case Option::Position: return options_.timingPosition;
    case Option::Mirror: return options_.randomMode;
    case Option::Score: return options_.exScore;
    case Option::SCritical: return options_.sCritical;
    case Option::TimingColor: return options_.timingColor;
    case Option::Rate: return options_.ultra ? 2 : options_.excessive;
    case Option::Ars: return options_.ars;
    case Option::LeftColor: return options_.leftColor;
    case Option::RightColor: return options_.rightColor;
    case Option::Retry: return options_.autoRetry;
    case Option::Constant: return options_.constantSpeed;
    case Option::TimingMs: return options_.timingMsStyle;
    case Option::SystemVoice: return options_.systemVoice;
    case Option::ValkyrieEffect: return options_.valkyrieEffect;
    case Option::Rank: return options_.showRank;
    case Option::Average: return options_.average;
    case Option::Challenge: return options_.challenge;
    case Option::Target: return options_.target;
    case Option::BestScore: return options_.bestScore;
    case Option::AutoPlay: return options_.autoPlay;
    case Option::Nemsys: return options_.nemsys;
    case Option::LanePreview: return lanePreview_.has_value();
    case Option::Count: break;
    }
    return 0;
}

std::string Cabinet::optionLabel(Option option) const {
    const int value = optionValue(option);
    char formatted[24];
    switch (option) {
    case Option::Speed: std::snprintf(formatted, sizeof(formatted), "%.2f", value / 100.); return formatted;
    case Option::Judgment: case Option::Display:
        return options_.fineTiming[option == Option::Display] ? (value > 0 ? "+" : "") + std::to_string(value) + "ms"
                                                             : std::to_string(value);
    case Option::Brightness: return std::to_string(value) + "%";
    case Option::Timing: return std::array{"OFF", "NEAR", "CRITICAL", "ALL"}[value];
    case Option::Position: return std::array{"STANDARD", "LOWER", "UPPER+", "UPPER", "OFF"}[value];
    case Option::Score: return value ? "EX SCORE" : "NORMAL";
    case Option::Rate: return std::array{"EFFECTIVE", "EXCESSIVE", "ULTRA"}[value];
    case Option::LeftColor: case Option::RightColor: return std::array{"BLUE", "RED", "GREEN", "YELLOW"}[value];
    default: return value ? "ON" : "OFF";
    }
}

void Cabinet::adjust(Option option, int steps) {
    if (page_ == Page::Settings && settingsReturn_ != Page::Room && sessionType_ == StartType::Light
        && (option == Option::Ars || option == Option::Score || option == Option::SCritical)) return;
    const int value = optionValue(option) + steps * (option == Option::Brightness ? 25 : 1);
    switch (option) {
    case Option::Speed: options_.speed = std::clamp(value, 100, 1000) / 100.; break;
    case Option::Judgment: case Option::Display: options_.setTimingValue(option == Option::Display, value); break;
    case Option::Brightness: options_.brightness = std::clamp(value, 50, 100); break;
    case Option::Timing: options_.cueStyle = std::clamp(value, 0, 3); break;
    case Option::Position: options_.timingPosition = std::clamp(value, 0, 4); break;
    case Option::Mirror:
        options_.randomMode = std::clamp(value, 0, 4);
        options_.mirror = options_.randomMode == 2 || options_.randomMode == 3;
        break;
    case Option::Score: options_.exScore = value > 0; break;
    case Option::SCritical:
        options_.sCritical = value > 0;
        if (!options_.sCritical) options_.ultra = false;
        break;
    case Option::TimingColor: options_.timingColor = value > 0; break;
    case Option::Rate:
        options_.excessive = value > 0;
        options_.ultra = value >= 2 && optionChoices(option) == 3;
        break;
    case Option::Ars: options_.ars = value > 0; break;
    case Option::LeftColor: options_.leftColor = std::clamp(value, 0, 3); break;
    case Option::RightColor: options_.rightColor = std::clamp(value, 0, 3); break;
    case Option::Retry: options_.autoRetry = value > 0; break;
    case Option::Constant: options_.constantSpeed = value > 0; break;
    case Option::TimingMs: options_.timingMsStyle = std::clamp(value, 0, 3); break;
    case Option::SystemVoice:
        options_.systemVoice = value > 0;
        audio_.setNarrationEnabled(options_.systemVoice);
        break;
    case Option::ValkyrieEffect: options_.valkyrieEffect = value > 0; break;
    case Option::Rank: options_.showRank = value > 0; break;
    case Option::Average: options_.average = value > 0; break;
    case Option::Challenge: options_.challenge = std::clamp(value, 0, 3); break;
    case Option::Target: options_.target = std::clamp(value, 0, 4); break;
    case Option::BestScore: options_.bestScore = std::clamp(value, 0, 2); break;
    case Option::AutoPlay: options_.autoPlay = value > 0; break;
    case Option::Nemsys: options_.nemsys = wrap(value, NemsysSkinCount); break;
    case Option::LanePreview:
        if (value > 0 && audio_.preview("selection_ui/audio/lanespeed.flac", 1, true)) {
            lanePreview_.emplace();
            resetInput();
        }
        break;
    case Option::Count: break;
    }
}

int Cabinet::optionChoice(Option option) const {
    if (option == Option::Brightness) return (100 - options_.brightness) / 25;
    if (option == Option::Position) return std::array{2,3,0,1,4}[options_.timingPosition];
    if (option == Option::Rate) return std::min(optionValue(option), optionChoices(option) - 1);
    return optionValue(option);
}

int Cabinet::optionChoices(Option option) const {
    if (option == Option::Rate)
        return options_.sCritical && (page_ != Page::Settings || settingsReturn_ == Page::Room
                                      || sessionType_ != StartType::Light) ? 3 : 2;
    if (option == Option::Position) return 5;
    if (option == Option::Brightness || option == Option::BestScore) return 3;
    if (option == Option::Timing || option == Option::TimingMs || option == Option::Challenge) return 4;
    if (option == Option::Mirror || option == Option::Target) return 5;
    if (option == Option::LeftColor || option == Option::RightColor) return 4;
    return 2;
}

void Cabinet::chooseOption(Option option, int choice) {
    int value = choice;
    if (option == Option::Brightness) value = 100 - choice * 25;
    if (option == Option::Position) value = std::array{2,3,0,1,4}[choice];
    adjust(option,(value-optionValue(option))/(option == Option::Brightness ? 25 : 1));
    play("menu/audio/cursor/se_04_cursor005_submonitor2.wav");
}

std::string_view Cabinet::optionArtwork(Option option, int choice) const {
    switch(option) {
    case Option::Position: return std::array{"upper_plus","upper","standard","lower","option_off"}[choice];
    case Option::Timing: case Option::TimingMs: return std::array{"early_late_off","early_late_near","early_late_critical","early_late_all"}[choice];
    case Option::Brightness: return std::array{"bg_100per","bg_75per","bg_50per"}[choice];
    case Option::Mirror: return std::array{"option_off","random","mirror","mirror_rondom","rondom_s"}[choice];
    case Option::Challenge: return std::array{"option_off","challenge_target","challenge_puc","challenge_uc"}[choice];
    case Option::Target: return std::array{"challenge_matching","challenge_hiscore","challenge_s","challenge_aaaplus","challenge_aaa"}[choice];
    case Option::BestScore: return std::array{"best_score_new","best_score_all","best_score_all_konasute"}[choice];
    case Option::Rate: return std::array{"effective_rate", "excessive_rate", "ultra_rate"}[choice];
    case Option::LeftColor: case Option::RightColor:
        return std::array{"panel_linecoler_blue","panel_linecoler_red","panel_linecoler_green","panel_linecoler_yellow"}[choice];
    case Option::Score: return "score_style";
    case Option::Constant: return "fix";
    default: return choice ? "option_on" : "option_off";
    }
}

void Cabinet::input(const GameInput& input) {
    if (startup_) return;
    using Type = GameInput::Type;
    if (input.type == Type::FocusLost) {
        bindTarget_ = -1;
        resetInput();
        return;
    }
    if (input.type == Type::TouchCancel) {
        if (finger_ == input.finger) resetInput();
        return;
    }
    if (!displayReady_ || (page_==Page::Songs && songOverlayOutAt_>=0) || (page_ == Page::Pin && entryNext_)
        || (page_ == Page::Credit && paymentAcceptedAt_ >= 0 && (paymentReceiptAt_ < 0 || paymentReceiptOutAt_ >= 0))
        || transition_ || roomDepartureAt_ >= 0 || page_ == Page::Stage || (page_ == Page::Exit && exitDecidedAt_ > 0)
        || (page_ == Page::Settings && (settingsLeavingAt_ >= 0 || now_ - entered_ < settingsDuration("in")))
        || (page_ == Page::Crew && crewLeavingAt_ >= 0)
        || (page_ == Page::Generator && generatorPhase_ >= 2)) return;
    if (lanePreview_) {
        bool exit = input.type == Type::Back;
        if (input.type == Type::TouchUp)
            exit = menuTarget(input.x, input.y) != nullptr;
        if (input.type == Type::Rotary || input.type == Type::Scroll) {
            if (held_[Start]) {
                knobRemainder_[0] += input.left + input.right;
                const int steps = static_cast<int>(knobRemainder_[0] / 20);
                knobRemainder_[0] -= steps * 20;
                adjust(Option::Speed, steps * 5);
            }
        } else if (input.type == Type::KeyDown || input.type == Type::KeyUp) {
            const bool down = input.type == Type::KeyDown;
            if (input.button >= 0) {
                const auto button = input.button;
                const bool pressed = down && !held_[button] && !input.repeat;
                held_[button] = down;
                if (pressed && button <= BTD) {
                    lanePreview_->update(audio_.previewPositionMs());
                    lanePreview_->press(button, now_, input.time, options_);
                }
                exit |= pressed && (button == Back || (held_[FXL] && held_[FXR]));
            } else if (down && !input.repeat) {
                switch (input.key) {
                case SDL_SCANCODE_KP_2: adjust(Option::Judgment, -1); break;
                case SDL_SCANCODE_KP_8: adjust(Option::Judgment, 1); break;
                case SDL_SCANCODE_KP_3: adjust(Option::Display, -1); break;
                case SDL_SCANCODE_KP_9: adjust(Option::Display, 1); break;
                default: break;
                }
            }
        }
        if (exit) {
            lanePreview_.reset();
            audio_.stopPreview(true);
            resetInput();
            saveProfile();
        }
        return;
    }
    if (messageUntil_ > now_) {
        if (input.type == Type::Back || input.type == Type::TouchUp || (input.type == Type::KeyDown && !input.repeat)) {
            messageUntil_ = 0;
            resetInput();
        }
        return;
    }
    if (achievementPopupAt_ >= 0) {
        if (input.type == Type::Back || input.type == Type::TouchUp || (input.type == Type::KeyDown && !input.repeat)) {
            achievementPopupAt_ = -1;
            resetInput();
        }
        return;
    }
    if (wantsTextInput()) {
        if (input.type == Type::Composition) { searchComposition_ = input.text; return; }
        if (input.type == Type::Text) {
            searchQuery_ += input.text;
            searchComposition_.clear();
            searchSongs();
            return;
        }
        if (input.type == Type::KeyDown) {
            if (input.key == SDL_SCANCODE_BACKSPACE && !searchQuery_.empty()) {
                size_t end = searchQuery_.size() - 1;
                while (end && (static_cast<unsigned char>(searchQuery_[end]) & 0xc0) == 0x80) --end;
                searchQuery_.erase(end);
                searchSongs();
            } else if (input.key == SDL_SCANCODE_RETURN || input.key == SDL_SCANCODE_KP_ENTER) {
                searchEditing_ = false;
                searchComposition_.clear();
            }
            // Typing controller-bound letters must never trigger game actions.
            return;
        }
        if (input.type == Type::KeyUp) return;
        if (input.type == Type::Back) { searchEditing_ = false; searchComposition_.clear(); return; }
    }
    if (input.type == Type::Back) {
        back();
        return;
    }
    if (input.type == Type::Rotary || input.type == Type::Scroll) {
        const std::array movement{input.left, input.right};
        if (page_ == Page::Matching) {
            if (!held_[Start]) return;
            startUsed_ |= movement[0] != 0 || movement[1] != 0;
        }
        if (controllerSettings()) {
            for (int side = 0; side < 2; ++side)
                calibration_[side] = std::clamp(calibration_[side] + movement[side], -360.f, 360.f);
        }
        for (int side = 0; side < 2; ++side) {
            knobRemainder_[side] += movement[side];
            int steps = static_cast<int>(knobRemainder_[side] / 20);
            knobRemainder_[side] -= steps * 20.f;
            rotate(side, steps);
        }
        return;
    }
    if (input.type == Type::TouchDown) {
        if (!finger_) {
            finger_ = input.finger;
            dragAnchor_ = touchStart_ = {input.x, input.y};
            touchDownAt_ = now_;
            dragged_ = false;
            if (page_ == Page::Settings) {
                auto* target = menuTarget(input.x, input.y);
                if (target && target->GetAttribute<std::string>("data-action", "") == "gauge")
                    gaugeDrag_ = target;
            }
            if (page_ == Page::Crew && roomTab_ == 3 && decorationTab_ == 2
                && input.x >= StickerCanvas.x && input.x < StickerCanvas.x + StickerCanvas.w
                && input.y >= StickerCanvas.y && input.y < StickerCanvas.y + StickerCanvas.h) {
                const int column = int((input.x - StickerCanvas.x) / StickerCell);
                const int row = int((input.y - StickerCanvas.y) / StickerCell);
                activeSticker_ = -1;
                for (int i = 0; i < int(draftStickers_.size()); ++i) {
                    const auto& sticker = draftStickers_[i];
                    const auto& art = boardStickers_[sticker.index];
                    if (column < sticker.column || column >= sticker.column + art.columns
                        || row < sticker.row || row >= sticker.row + art.rows) continue;
                    activeSticker_ = i;
                    stickerDragOrigin_ = {sticker.column, sticker.row};
                    stickerDragging_ = true;
                    break;
                }
            }
        }
        return;
    }
    if (input.type == Type::TouchMove || input.type == Type::TouchUp) {
        if (finger_ != input.finger) return;
        if (input.type == Type::TouchMove) {
            if (stickerDragging_) {
                auto& sticker = draftStickers_[activeSticker_];
                const auto& art = boardStickers_[sticker.index];
                PlacedSticker moved{sticker.index,
                    std::clamp(stickerDragOrigin_[0] + int((input.x - touchStart_.x) / StickerCell), 0, StickerColumns - art.columns),
                    std::clamp(stickerDragOrigin_[1] + int((input.y - touchStart_.y) / StickerCell), 0, StickerRows - art.rows)};
                for (int i = 0; i < int(draftStickers_.size()); ++i) {
                    if (i == activeSticker_) continue;
                    const auto& other = draftStickers_[i];
                    const auto& otherArt = boardStickers_[other.index];
                    if (moved.column < other.column + otherArt.columns && other.column < moved.column + art.columns
                        && moved.row < other.row + otherArt.rows && other.row < moved.row + art.rows) {
                        moved.column = moved.column < other.column + otherArt.columns / 2
                            ? other.column - art.columns : other.column + otherArt.columns;
                    }
                }
                if (stickerFits(moved, draftStickers_, activeSticker_)) sticker = moved;
                return;
            }
            if (page_==Page::Songs) {
                const float dx=input.x-touchStart_.x, dy=input.y-touchStart_.y;
                if (!songConfirm_ && !dragged_ && std::abs(dx)>80 && std::abs(dx)>std::abs(dy)*1.5f
                    && ((sortOpen_ && dx>0) || (!sortOpen_ && touchStart_.x>=980 && dx<0))) {
                    toggleWheel(Wheel::Sort); dragged_=true; return;
                }
                if (sortOpen_ || songConfirm_) {
                    const float delta=sortOpen_?dragAnchor_.y-input.y:input.x-dragAnchor_.x;
                    const float unit=sortOpen_?57.f:105.f;
                    const int steps=int(delta/unit);
                    if (steps) {
                        if (sortOpen_) dragAnchor_.y-=steps*unit; else dragAnchor_.x+=steps*unit;
                        rotate(sortOpen_ ? 1 : 0,steps); dragged_=true;
                    }
                    return;
                }
            }
            if (cardFilterOpen_) return;
            if (page_ == Page::Crew && roomTab_ == 1 && touchStart_.x >= CrewGrid.x && touchStart_.y >= CrewGrid.y) {
                const int rows = int((dragAnchor_.y - input.y) / 140);
                if (rows) {
                    dragAnchor_.y -= rows * 140;
                    const int last = std::max(0, (int(crewFamilies().size()) - 1) / 5 * 5 - 20);
                    crewOffset_ = std::clamp(crewOffset_ + rows * 5, 0, last);
                    dragged_ = true;
                }
                return;
            }
            if ((page_ == Page::Crew && roomTab_ == 4) || (page_ == Page::Settings && playTab_ == 3)) {
                const int steps = int((dragAnchor_.y - input.y) / 80);
                if (steps && touchStart_.y >= 331) {
                    dragAnchor_.y -= steps*80;
                    if (touchStart_.x < 560) achievementCategoryOffset_ = std::clamp(achievementCategoryOffset_+steps,0,int(AchievementFolders.size())-8);
                    else rotate(1,steps);
                    dragged_ = true;
                }
                return;
            }
            const bool roomList = page_ == Page::Crew && (roomTab_ == 2 || roomTab_ == 3)
                && touchStart_.x >= 1072 && touchStart_.y >= 276;
            const bool searchList = page_ == Page::Settings && playTab_ == 2 && touchStart_.x >= 879 && touchStart_.y >= 216;
            if (roomList || searchList) {
                const float stepSize = roomList && roomTab_ == 3 && decorationTab_ == 1 ? 140.f : 91.f;
                const int steps = int((dragAnchor_.y - input.y) / stepSize);
                if (steps) {
                    dragAnchor_.y -= steps * stepSize;
                    if (searchList) rotate(1, steps * 5);
                    else if (roomTab_ == 2) cardOffset_ = std::clamp(cardOffset_ + steps*5, 0,
                        std::max(0, (int(visibleCards_.size()) - 1) / 5 * 5 - 15));
                    else if (decorationTab_ == 1) decorationOffset_ = std::clamp(decorationOffset_ + steps, 0, std::max(0, int(boardBases_.size())-2));
                    else if (decorationTab_ == 2) decorationOffset_ = std::clamp(decorationOffset_ + steps*5, 0,
                        std::max(0, (int(boardStickers_.size()) - 1) / 5 * 5 - 15));
                    dragged_ = true;
                }
                return;
            }
            const bool vertical = (page_ == Page::Songs && touchStart_.x >= 510 && touchStart_.y > 160 && touchStart_.y < 1700)
                || page_ == Page::Mode || page_ == Page::Skill;
            const bool horizontal = page_ == Page::Songs && touchStart_.x < 510 && touchStart_.y > 250;
            if (vertical || horizontal) {
                const float delta = vertical ? dragAnchor_.y - input.y : dragAnchor_.x - input.x;
                const float stepSize = vertical ? 92 : 105;
                const int steps = static_cast<int>(delta / stepSize);
                if (steps) {
                    if (vertical) dragAnchor_.y -= steps * stepSize;
                    else dragAnchor_.x -= steps * stepSize;
                    rotate(page_ == Page::Settings ? 0 : (vertical ? 1 : 0), steps);
                    dragged_ = true;
                }
            }
            if (gaugeDrag_) {
                setGauge(*gaugeDrag_, input.y);
                dragged_ = true;
            }
            return;
        }
        finger_.reset();
        gaugeDrag_ = nullptr;
        if (stickerDragging_) { stickerDragging_ = false; return; }
        if (!dragged_) touch(input.x, input.y);
        return;
    }

    const bool down = input.type == Type::KeyDown;
    const int key = input.key;
    if (controllerSettings() && bindTarget_ >= 0) {
        if (!down || input.repeat) return;
        const bool joystick = controllerMode_ == ControllerMode::Joystick;
        if (key == SDL_SCANCODE_ESCAPE && (joystick || bindTarget_ != Back)) {
            bindTarget_ = -1;
        } else if (const int binding = joystick ? input.controllerKey : key; binding > 0) {
            auto& keys = joystick ? controllerMap_ : keyMap_;
            const auto previous = std::find(keys.begin(), keys.end(), binding);
            if (previous != keys.end()) *previous = keys[bindTarget_];
            keys[bindTarget_] = binding;
            play(Decide);
            bindTarget_ = -1;
            resetInput();
            saveProfile();
        }
        return;
    }
    if (down && !input.repeat) {
        if (input.button < 0 && page_ == Page::Pin && entryStep_ == EntryStep::Pin) {
            int digit = -1;
            if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9) digit = key - SDL_SCANCODE_1 + 1;
            else if (key >= SDL_SCANCODE_KP_1 && key <= SDL_SCANCODE_KP_9) digit = key - SDL_SCANCODE_KP_1 + 1;
            else if (key == SDL_SCANCODE_0 || key == SDL_SCANCODE_KP_0) digit = 0;
            if (digit >= 0 || key == SDL_SCANCODE_BACKSPACE) {
                constexpr std::array digitKeys{9, 6, 7, 8, 3, 4, 5, 0, 1, 2};
                enterPinKey(digit < 0 ? 11 : digitKeys[digit]);
                return;
            }
        }
        if (input.button < 0 && (key == SDL_SCANCODE_F2 || key == SDL_SCANCODE_KP_PLUS)) {
            credit();
            return;
        }
        if (input.button < 0 && (key == SDL_SCANCODE_KP_00 || key == SDL_SCANCODE_KP_0) && sessionType_ == StartType::Premium
            && !practiceExpired() && (page_ == Page::Matching || page_ == Page::Result)) {
            options_.autoRetry = !options_.autoRetry;
            saveProfile();
            play(Cursor);
            return;
        }
        if (input.button < 0 && key == SDL_SCANCODE_KP_0 && (page_ == Page::Matching || page_ == Page::Result)) {
            characterView_ = !characterView_;
            return;
        }
        if (input.button < 0 && (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_AC_BACK)) {
            back();
            return;
        }
    }
    if (input.button >= 0) {
        const int button = input.button;
        const bool wasHeld = held_[button];
        held_[button] = down;
        if (button == Subscreen) {
            if (down && !wasHeld && !input.repeat) {
                if (page_ == Page::Room || page_ == Page::Matching ||
                    (page_ == Page::Songs && !songConfirm_ && !sortOpen_)) openSettings();
                else if (page_ == Page::Settings) back();
            }
            return;
        }
        if (button <= BTD && down && !wasHeld && !input.repeat && canEndPremium()
            && held_[BTA] && held_[BTB] && held_[BTC] && held_[BTD]) {
            requestPremiumExit();
            return;
        }
        if (button == Back && down && !input.repeat) {
            back();
            return;
        }
        if (button == FXL || button == FXR) {
            if (down && !wasHeld && held_[FXL] && held_[FXR]) {
                fxCombined_ = true;
                if (page_==Page::Songs && songConfirm_) closeSongOverlay(false);
                else if (page_ == Page::Songs && !sortOpen_) openSettings();
                else if (page_ == Page::Matching) openSettings();
                else if (page_ == Page::Room) enter(Page::Crew);
                else if (page_ == Page::Settings) back();
                else if (page_ == Page::Generator || page_ == Page::Valkyrie) go(Page::Room);
                return;
            }
            if (!down && wasHeld && !held_[FXL] && !held_[FXR]) {
                if (!fxCombined_) {
                    if (page_ == Page::Songs) {
                        if (!songConfirm_ && (!sortOpen_ || button == FXR))
                            toggleWheel(button == FXL ? Wheel::Folder : Wheel::Sort);
                    }
                    else if (page_ == Page::Settings) rotate(0, button == FXL ? -1 : 1);
                    else if (page_ == Page::Crew && button == FXL) cancelCrew();
                    else if (button == FXL && page_ != Page::Pin && page_ != Page::Generator
                             && page_ != Page::Valkyrie && page_ != Page::Result) back();
                }
                fxCombined_ = false;
            }
            return;
        }
        if (button == Start) {
            if (page_ == Page::Matching) {
                if (down && !wasHeld) {
                    startUsed_ = false;
                    knobRemainder_.fill(0);
                }
                if (!down && wasHeld) {
                    if (startUsed_) saveProfile();
                    else confirm();
                }
            } else if (page_ == Page::Result && sessionType_ == StartType::Premium) {
                if (down && !wasHeld && !input.repeat) {
                    if (practiceExpired() || resultVoicePending() || !forceDone_) confirm();
                    else resultRetryAt_ = std::min(input.time, now_);
                } else if (!down && wasHeld && resultRetryAt_ >= 0) {
                    const double duration = std::min(input.time, now_) - resultRetryAt_;
                    resultRetryAt_ = -1;
                    if (!practiceExpired() && duration >= PremiumRetryHoldMs) go(Page::Matching);
                    else confirm();
                }
            } else if (down && !input.repeat) confirm();
            return;
        }
        if (button < FXL && down && page_ == Page::Settings) {
            rotate(button < BTC ? 0 : 1, (button == BTA || button == BTC) ? -1 : 1);
            return;
        }
        if ((button == BTA || button == BTB) && down && !input.repeat && page_ == Page::Crew
            && roomTab_ == 3 && decorationTab_ == 2 && !stickerDragging_) {
            if (button == BTA) placeSticker(std::max(0, stickerFocus_));
            else removeSticker();
            return;
        }
        if ((button == BTC || button == BTD) && down && !input.repeat && page_ == Page::Crew) {
            rotate(1, button == BTC ? -1 : 1);
            return;
        }
    }
    if (!down || input.button >= 0) return;
    if (!input.repeat && (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER)) {
        confirm();
        return;
    }
    if (page_ == Page::Settings && playTab_ == 1 && settingsTab_ == SettingsTab::General) {
        switch (key) {
        case SDL_SCANCODE_KP_2: adjust(Option::Judgment, -1); return;
        case SDL_SCANCODE_KP_8: adjust(Option::Judgment, 1); return;
        case SDL_SCANCODE_KP_3: adjust(Option::Display, -1); return;
        case SDL_SCANCODE_KP_9: adjust(Option::Display, 1); return;
        default: break;
        }
    }
    switch (key) {
    case SDL_SCANCODE_LEFT: rotate(sortOpen_ ? 1 : 0, -1); break;
    case SDL_SCANCODE_RIGHT: rotate(sortOpen_ ? 1 : 0, 1); break;
    case SDL_SCANCODE_UP: rotate(1, page_ == Page::Settings || page_ == Page::Matching ? 1 : -1); break;
    case SDL_SCANCODE_DOWN: rotate(1, page_ == Page::Settings || page_ == Page::Matching ? -1 : 1); break;
    default: break;
    }
}

}
