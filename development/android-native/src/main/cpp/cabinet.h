#pragma once

#include <span>

#include "app.h"
#include "cabinet_rewards.h"
#include "cabinet_navigator.h"
#include "stage.h"
#include <algorithm>
#include <vector>

namespace Rml { class Element; class ElementDocument; }

namespace sdvx {
class Cabinet {
public:
    Cabinet(Render& render, Audio& audio, bool showStartup = false);
    ~Cabinet();
    void update(double nowMs);
    void draw(double nowMs);
    void preload();
    void input(const GameInput& input);
    std::optional<Selection> takeSelection();
    const Selection* stagePreparation() const;
    bool stageTransition() const;
    bool matchingDeparture() const { return transition_ && transition_->kind == TransitionKind::MatchingOut; }
    bool stageDepartureComplete(double nowMs) const;
    bool stageClosing(double nowMs) const;
    void stageLoaded(double nowMs);
    void drawStageShutter(double nowMs);
    const PlayOptions& options() const { return stageOptions_; }
    PlayerCard playerCard(bool draft = false) const;
    int bestScore(const Selection& track) const { return record(track).score; }
    void applyStageOptions(const PlayOptions&);
    void stageRetried() { ++quickRetries_; }
    void stageFinished(const Result& result, double nowMs);
    int mappedButton(int key) const;
    int mappedControllerButton(int key) const;
    void setControllerMode(ControllerMode mode) { controllerMode_ = mode; calibration_.fill(0); resetInput(); }
    float rotaryDelta(int side, float dx, float dy) const;
    bool landscape() const { return page_ == Page::Crew || (page_ == Page::Settings && !lanePreview_); }
    void setDisplayReady(bool ready, double now);
    void releaseGraphics();
    void restoreGraphics();
    bool wantsTextInput() const { return page_ == Page::Settings && playTab_ == 2 && searchEditing_; }
    unsigned keyboardRequest() const { return keyboardRequest_; }
    void setKeyboardTop(float y) { keyboardBarTop_ = std::clamp(y-72.f,120.f,588.f); }

private:
    static constexpr auto Cursor = "crew/cursor.wav";
    static constexpr auto Decide = "menu/audio/se_05_decide002_medium.wav";
    static constexpr std::string_view StartupWarning = "startup/warning_j.png";
    static constexpr std::string_view StartupKonami = "startup/konami_red50.png";
    static constexpr std::string_view StartupAmusement = "startup/e-amu.png";
    static constexpr double KonamiLogoAt = 2850, StartupEnd = 7800;
    struct Startup {
        double started = -1;
        std::future<void> catalogs;
        bool profileReady = false;
        bool logoPlayed = false;
    };
    void loadCatalogs();
    void prepareProfile();
    void openDocument();
    void closeDocuments();
    void updateMenuInput();
    Rml::Element* menuTarget(float x, float y);
    void menuAction(std::string_view action, int value);
    void setGauge(Rml::Element& gauge, float y);
    void useField(Rml::Element& element);
    void setUiText(Rml::Element& element, std::string_view value);
    void drawStartup(double elapsed);

    enum class Page {
        Title, Pin, Room, Generator, Valkyrie, Crew, Caution, Mode, Credit, Skill, Songs,
        Matching, Stage, Result, Summary, Exit, Saved, GameEnd, Settings
    };
    static constexpr std::array RoomPages{Page::Generator, Page::Caution, Page::Valkyrie};
    static constexpr std::array AchievementFolders{24,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,25,26,27};
    static constexpr std::array<Rect, 3> RoomTiles{{{0, 1110, 372, 372}, {355, 1012, 372, 372}, {710, 1110, 372, 372}}};
    static constexpr Rect CrewPanel{0, 0, 1920, 1080};
    static constexpr Rect CrewGrid{1124,376,700,700};
    static constexpr std::array<std::string_view, 12> PinKeys{"7", "8", "9", "4", "5", "6", "1", "2", "3", "0", "00", "←"};
    static constexpr std::string_view ModeLetters = "efgabdhi";
    static constexpr int ModeCount = ModeLetters.size();
    static constexpr int NormalMode = 3, PremiumMode = 2, SkillMode = 1, DefaultPaseli = 114514;
    static constexpr double PinTime = 20000, ModeTime = 60000, RoomTime = 90000, SongTime = 60000, ResultTime = 30000, SummaryTime = 12000, ContinueTime = 10000;
    static constexpr double SkillChangeAt = 220000.0 / 120, SkillTitleTime = 40000;
    static constexpr double ResultGradeAt = 300;
    static constexpr double SkillBonusWait = 500000.0 / 120, SkillBonusResultWait = 1500000.0 / 120;
    enum class StartType { Light, Standard, Premium, Skill };
    enum class PinPhase { Create, Repeat, Verify };
    enum class EntryStep { CardRead, Registration, Transfer, Terms, Pin, Code, Name, Loading, Profile, PaymentCheck, PaymentNotice, ModeSelect };
    static constexpr std::string_view NameCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !?#$&*.-";
    enum class Wheel { Songs, Folder, Sort };
    enum class FolderKind {
        All, Kana, Level, LevelAll, LevelExact, Grades, Grade, NearGrades, NearGrade,
        Lamps, Lamp, Genres, Genre, Contests, Contest, Versions, Version, Dates, Date,
        SkillLevels, SkillLevel, Volforce, ForcePotential, EffectRadar, EffectAxis,
        PlayerRadar, PlayerAxis, Played, Favorites, ExChallenge, Beginner
    };
    struct SongFolder {
        FolderKind kind;
        int value;
        std::string name;
        int count = 0;
        std::array<std::pair<int, int>, 6> badges{};
        bool child = false, expandable = false;
    };
    static constexpr std::array<std::string_view,7> SongSorts{
        "デフォルト", "曲名順", "アーティスト名順", "レベル順", "配信日順", "クリアマーク順", "スコアグレード順"};
    enum class Option {
        Speed, Judgment, Display, Brightness, Timing, Position, Mirror, Score, SCritical,
        TimingColor, Rate, Ars, LeftColor, RightColor, Retry, Constant,
        TimingMs, SystemVoice, ValkyrieEffect, Rank, Average, Challenge, Target, BestScore, AutoPlay, Nemsys,
        LanePreview, Count
    };
    static constexpr Rect LanePreviewOption{1312,949,230,46};
    static constexpr Rect ArsOption{666,413,198,48};
    static constexpr int ControllerTabFocus = static_cast<int>(Option::Count);
    enum class SettingsTab { General, Controller };
    bool controllerSettings() const {
        return page_ == Page::Settings && playTab_ == 1 && settingsTab_ == SettingsTab::Controller;
    }
    static constexpr Rect GeneralSettingsTab{608,138,304,54}, ControllerSettingsTab{1008,138,304,54};
    enum BindingControl { Axes = ButtonCount, ReverseLeft, ReverseRight, ResetBindings, BindingControlCount };
    struct OptionSlot { Option option; Rect bounds; const char* popup; };
    // Original page_option coordinates on the 1920x1080 submonitor.
    static constexpr std::array<OptionSlot, 19> OptionSlots{{
        {Option::Position,{181,320,196,46},"option_early_late_position"},
        {Option::Timing,{667,320,196,46},"option_early_late_style"},
        {Option::TimingColor,{909,320,196,46},""},
        {Option::Brightness,{181,466,196,46},"option_bg"},
        {Option::Mirror,{424,466,196,46},"option_mirror"},
        {Option::Rate,{667,466,196,46},"option_panel_ratestyle"},
        {Option::LeftColor,{904,466,96,48},"option_linecolor"},
        {Option::RightColor,{1014,466,96,48},"option_linecolor"},
        {Option::SCritical,{181,613,196,46},""},
        {Option::Score,{181,758,196,46},""},
        {Option::TimingMs,{424,320,196,46},"option_plusminus_ms_style"},
        {Option::SystemVoice,{424,613,196,46},""},
        {Option::ValkyrieEffect,{667,613,196,46},""},
        {Option::Rank,{909,613,196,46},""},
        {Option::Average,{424,905,196,46},""},
        {Option::Challenge,{424,758,196,46},"option_challenge"},
        {Option::Target,{667,758,196,46},"option_target"},
        {Option::BestScore,{909,758,196,46},"option_best_score"},
        {Option::AutoPlay,{667,905,196,46},""}
    }};
    struct Chart {
        Selection track;
        std::string recordKey;
        std::array<int, 6> radar{};
        bool installed = true;
    };
    struct Song {
        int version;
        int challenge = -1;
        std::vector<Chart> charts;
        std::string searchKey;
        std::string titleKey, artistKey;
        int date = 0, genre = 0, kana = 10;
        std::vector<int> contests;
        bool beginner = false;
    };
    struct Record {
        int score = 0;
        std::string grade, outcome;
        int force = 0;
        std::array<int, 6> radar{};
    };
    struct AppealCard {
        int id;
        std::string image, thumbnail, name, illustrator;
        Rect thumbnailUv;
        std::array<std::string, 8> messages;
        int filterMask;
    };
    static constexpr int CardGenreMask = 0x3e, CardSeriesMask = 0x1fc0;
    static constexpr Rect CardFilterPanel{1100,246,740,600};
    static constexpr std::array<std::pair<int, std::string_view>, 12> CardFilterOptions{{
        {0x002,"オリジナル"}, {0x008,"東方"}, {0x010,"GUMI"}, {0x004,"ひなビタ♪"}, {0x020,"その他"},
        {0x040,"BOOTH"}, {0x080,"INFINITE"}, {0x100,"GRAVITY"}, {0x200,"HEAVENLY"},
        {0x400,"VIVID"}, {0x800,"EXCEED"}, {0x1000,"NABLA"},
    }};
    static constexpr Rect cardFilterOptionBounds(int index) {
        const bool series = index >= 5;
        const int slot = series ? index - 5 : index;
        return {CardFilterPanel.x + 35.55f + (slot % 3) * 230,
                CardFilterPanel.y + (series ? 242.2f : 125.2f) + (slot / 3) * 37, 210, 30};
    }
    struct Decoration {
        int id;
        std::string image, thumbnail{}, name{};
        int columns = 0, rows = 0;
    };
    struct PlacedSticker {
        int index, column, row;
        bool operator==(const PlacedSticker&) const = default;
    };
    static constexpr int StickerCell = 25, StickerColumns = 24, StickerRows = 12;
    static constexpr Rect StickerCanvas{416, 300, StickerColumns * StickerCell, StickerRows * StickerCell};
    static constexpr Rect StickerWindow{1072, 246, 850, 774};
    struct Course { std::string name, set; int level, nameId, seasonId, id; std::vector<std::pair<int,int>> tracks; };
    enum class SkillBonusPhase { Pending, Waiting, Counting, Ready, Closing, Done };
    struct Achievement {
        int id, category, rewardType, reward, prerequisite, goal, parameter, progress = 0;
        std::string title, metric;
    };
    struct SessionTrack {
        Selection selection;
        Result result;
    };
    enum class TransitionKind { Shutter, TrackIntro, TrackEntry, NextArea, MatchingOut };
    enum class TransitionPhase { Covering, WaitingForStage, Opening };
    struct Transition {
        Page next;
        double started;
        double inMs;
        double holdMs;
        double outMs;
        TransitionKind kind;
        TransitionPhase phase = TransitionPhase::Covering;
        bool newTrack = false;
        double announcementInMs = 0;
        double announcementOutOffset = -1;
    };

    const Song& song() const { return songs_[visibleSongs_[songIndex_]]; }
    const Chart& chart() const;
    const Record& record(const Selection&) const;
    int totalForce() const;
    void rebuildSongs();
    std::pair<int, int> songListPosition() const;
    std::string songScrollLabel() const;
    void rebuildFolders(FolderKind group = FolderKind::All, int groupValue = 0);
    std::vector<std::pair<int, int>> folderRows(const SongFolder&) const;
    int folderIndex(FolderKind kind, int value = 0) const;
    static bool folderChartRows(FolderKind);
    std::vector<std::pair<std::string_view, int>> topForceCharts() const;
    static std::array<int, 6> playerRadar(const Chart&, int score, int exScore, int maxExScore);
    std::pair<int, int> chartRange(const Song&, bool challenge) const;
    bool exTrack() const { return sessionType_ == StartType::Standard && track_ == 4; }
    bool exChallengeFolder() const {
        return activeFolder_ >= 0 && songFolders_[activeFolder_].kind == FolderKind::ExChallenge;
    }
    void moveSongList(int steps);
    void startSongSelectionAudio();
    void closeSongOverlay(bool accept);
    int displayChart(const Song&) const;
    void loadProfile();
    void saveProfile() const;
    void refreshBlasterPass();
    bool blasterPassActive() const;
    void enter(Page next, bool newTrack = false);
    void go(Page next, bool track = false);
    void startTrackSelection();
    std::string_view trackIntroName() const;
    NavigatorCue trackIntroCue() const;
    void confirm();
    void startResultForce();
    const char* resultShineClip() const { return result_.outcome == "crash" ? "crash" : "in"; }
    bool resultVoicePending() const { return forceAt_ < 0 && !navigator_.readyForVoice(); }
    void back();
    void switchSettingsTab(SettingsTab tab);
    double settingsDuration(std::string_view clip) const;
    void activateBinding(int control);
    void rotate(int side, int steps);
    bool setRoomFocus(int focus);
    void touch(float x, float y);
    void credit();
    struct PaymentOffer {
        std::string_view titleArt;
        int price;
        bool paseli;
        std::optional<StartType> session;
    };
    static std::span<const PaymentOffer> modePayments(int mode);
    void startSession(const PaymentOffer& offer);
    double paymentModeOutAt() const;
    bool skillPassed() const;
    int skillAchievement() const;
    std::string_view skillResultIntro() const;
    double skillResultIntroDuration() const;
    int skillBonusRemaining(int amount) const;
    void finishSummaryCount();
    void enterPinKey(int index);
    void entryStep(EntryStep next);
    void advanceEntry(EntryStep next);
    std::string_view entryMovie() const;
    std::string_view entryFolder() const;
    void confirmEntry();
    void eraseNameCharacter();
    bool practiceExpired() const { return practiceUntil_ > 0 && now_ >= practiceUntil_; }
    bool canEndPremium() const;
    bool premiumExit() const;
    void requestPremiumExit();
    void toggleWheel(Wheel);
    void openSettings();
    void searchSongs();
    void rebuildCards();
    void switchRoomTab(int tab);
    std::string_view roomPageName() const;
    void resetInput();
    void chooseCrew(int index);
    int crewFamilyIndex() const;
    void keepCrewVisible();
    void cancelCrew();
    void commitCustomization();
    bool stickerFits(const PlacedSticker&, const std::vector<PlacedSticker>&, int ignored = -1) const;
    void placeSticker(int index);
    void removeSticker();
    void updateAchievements();
    bool achievementReady(const Achievement&) const;
    void claimAchievement(int index);
    void play(std::string_view path, float gain = .75f);
    void notice(std::string text);
    int optionValue(Option) const;
    std::string optionLabel(Option) const;
    void adjust(Option, int steps);
    int optionChoice(Option) const;
    int optionChoices(Option) const;
    void chooseOption(Option, int choice);
    std::string_view optionArtwork(Option, int choice) const;

    void drawScene(double elapsed);
    void flushLabels();
    void drawRoomArt();
    void drawRoom(double elapsed);
    void drawPin(double elapsed);
    void drawExit(double elapsed);
    void drawGenerator(double elapsed);
    void drawCrew(double elapsed);
    void drawMode(double elapsed);
    void prepareModeArtwork();
    void drawSkill(double elapsed);
    void drawCredit(double elapsed);
    void drawSongs(double elapsed);
    void drawSongOverlay();
    void drawMatching(double elapsed);
    void drawMatchingPanel(double elapsed, bool departing);
    void drawNextArea(double elapsed, bool leaving);
    void drawResult(double elapsed);
    void drawBlasterGauge(int energy, double elapsed, bool result);
    void drawSummary(double elapsed);
    void drawSkillTitleChange();
    void drawSkillBonus();
    void drawSettings(double elapsed);
    void drawSubscreenHeader(bool room);
    void drawCustomization(double elapsed);
    void drawProfile(Rect bounds, bool draft);
    void drawResultProfile(const char* id);
    void drawPlayerCard(Rect bounds, int style, bool draft);
    void drawSearch(double elapsed);
    void drawAchievements(double elapsed);
    void drawTransition(const Transition&, double elapsed);
    void header(std::string_view name);
    void footer(bool result = false);
    void radar(const Chart&, Vec2 center, float radius, bool labels = true);
    void level(int tenths, Rect bounds, bool selected);
    void field(const char* id, std::string_view value);
    void rowField(const char* id, int row, int column, std::string_view value);
    void drawUiElement(Rml::Element& element, float x = 0, float y = 0, float scale = 1);
    void drawUiLayers(bool updateLayout = true);
    void uiImage(Rml::Element& element, std::string_view path);
    void uiLayer(const char* id, std::string_view path = {});
    void art(std::string_view name, Rect bounds);
    void ap(std::string_view folder, std::string_view animation, std::string_view clip,
            float x, float y, double ms, bool loop = false, const Bindings& bindings = {});

    Render& render_;
    Audio& audio_;
    Navigator navigator_;
    Rml::ElementDocument* document_ = nullptr;
    Rml::ElementDocument* inputDocument_ = nullptr;
    Rml::Element* inputView_ = nullptr;
    Rml::Element* gaugeDrag_ = nullptr;
    std::vector<Rml::Element*> pendingFields_, visibleFields_;
    std::vector<Rml::Element*> uiLayers_;
    bool uiDirty_ = false;
    std::vector<Song> songs_;
    std::vector<Course> courses_;
    int courseIndex_ = 0;
    std::optional<int> activeCourse_;
    int skillLevel_ = 0, skillLevelBefore_ = 0;
    int skillTitle_ = 0, skillTitleBefore_ = 0, skillTitleFocus_ = 0;
    bool skillLevelChanged_ = false, skillChangePlayed_ = false, skillChangeEndPlayed_ = false;
    bool skillFailureSpoken_ = false, skillTitleHandled_ = false;
    double skillTitleAt_ = -1, skillTitleFocusAt_ = -1, skillTitleDecidedAt_ = -1;
    std::vector<std::pair<int, int>> clearedSkillCourses_;
    SkillBonusPhase skillBonusPhase_ = SkillBonusPhase::Pending;
    int skillBonus_ = 0, skillBonusPackets_ = 0, skillBonusBlocks_ = 0;
    double skillBonusAt_ = 0;
    std::vector<AppealCard> appealCards_;
    std::vector<Decoration> boardBases_, boardStickers_;
    std::vector<int> favoriteCards_;
    std::vector<int> visibleCards_;
    std::vector<Achievement> achievements_;
    std::vector<std::string> achievementCategories_;
    std::vector<int> claimedAchievements_, earnedTitles_;
    std::vector<int> generatedParts_;
    static constexpr int GeneratorPrice = 100;
    int generatorPhase_ = 0, generatorFocus_ = 1, generatorCount_ = 1;
    double generatorAt_ = 0;
    std::vector<std::pair<bool, std::string>> generatorRewards_;
    size_t generatorRewardIndex_ = 0;
    int achievementCategory_ = 24, achievementCategoryOffset_ = 0, achievementOffset_ = 0;
    int packets_ = 0, blocks_ = 0, playCount_ = 0, playDays_ = 0, dayChain_ = 0, weekChain_ = 0, todayPlays_ = 0;
    bool blasterPassActive_ = true;
    int64_t blasterPassLimitDate_ = 0, blasterPassUntil_ = 0;
    int blasterEnergy_ = 0;
    std::array<int, 6> exChallengeUnlocks_{};
    int64_t lastPlayDay_ = -1, lastPlayWeek_ = -1;
    double achievementPopupAt_ = -1;
    std::string achievementRewardText_;
    std::vector<int> visibleSongs_, versions_;
    std::vector<SongFolder> songFolders_;
    std::vector<std::string> floorContests_;
    std::vector<std::pair<std::string_view, int>> forceFolderCharts_;
    std::array<int, 6> playerRadarCutoffs_{};
    std::vector<int> favoriteSongs_, visibleCharts_;
    FolderKind folderGroup_ = FolderKind::All;
    int folderGroupValue_ = 0, activeFolder_ = -1, preferredChart_ = 0;
    bool songFoldersDirty_ = false;
    bool sortOpen_ = false, songConfirm_ = false, songConfirmAccept_ = false;
    bool songPromptPending_ = false;
    double songPromptAt_ = -1;
    int sortFocus_ = 0;
    double songOverlayAt_ = 0, songOverlayOutAt_ = -1;
    std::unordered_map<std::string, Record> records_;
    std::vector<SessionTrack> sessionResults_;
    Result result_;
    int previousBest_ = 0, resultBlasterBefore_ = 0, quickRetries_ = 0;
    bool resultRecordArtReady_ = false;
    bool resultChangedLamp_ = false;
    std::string_view resultShine_, resultGradeSound_;
    double resultShineDuration_ = 0;
    int forceBefore_ = 0, forceAfter_ = 0, forceEffect_ = 0;
    double forceAt_ = -1, resultRewardsAt_ = -1;
    bool forceDone_ = true, forceLevelSound_ = false;
    int sessionPackets_ = 0, sessionBlocks_ = 0;
    StageBlockAward resultBlocks_;
    bool sessionPaidWithPaseli_ = false;
    bool summaryCountDone_ = false, summaryCountPlaying_ = false;
    PlayOptions options_, stageOptions_;
    std::array<int, ButtonCount> keyMap_{};
    static constexpr std::array DefaultControllerKeys{1, 2, 3, 4, 5, 6, 9, 0, 0};
    std::array<int, ButtonCount> controllerMap_ = DefaultControllerKeys;
    ControllerMode controllerMode_ = ControllerMode::None;
    std::array<bool, ButtonCount> held_{};
    std::array<bool, 2> reverse_{};
    std::array<float, 2> knobRemainder_{};
    std::array<float, 2> calibration_{};
    std::optional<int64_t> finger_;
    Vec2 touchStart_{}, dragAnchor_{};
    bool dragged_ = false;
    bool leftUsesX_ = false, fxCombined_ = false, startUsed_ = false;
    bool controllerLeftUsesY_ = false;
    std::array<bool, 2> controllerReverse_{};
    double resultRetryAt_ = -1;
    std::optional<LanePreview> lanePreview_;
    int bindTarget_ = -1, bindingFocus_ = 0, optionFocus_ = 0, optionPopup_ = -1;
    int playTab_ = 1, searchFocus_ = 0;
    int roomTab_ = 0;
    int appealCard_ = 0, draftCard_ = 0, cardOffset_ = 0;
    int boardStyle_ = 0, draftStyle_ = 0, boardBase_ = 0, draftBase_ = 0;
    int decorationTab_ = 0, decorationOffset_ = 0, stickerFocus_ = -1, activeSticker_ = -1;
    bool stickerDragging_ = false;
    std::array<int, 2> stickerDragOrigin_{};
    std::vector<PlacedSticker> placedStickers_, draftStickers_, profileCacheStickers_;
    bool favoriteCardsOnly_ = false;
    bool cardFilterOpen_ = false;
    int cardFilter_ = 0, draftCardFilter_ = 0;
    std::array<int,5> profileCacheState_{{-2,-2,-2,-2,-2}};
    double touchDownAt_ = 0;
    SettingsTab settingsTab_ = SettingsTab::General;
    std::optional<SettingsTab> nextSettingsTab_;
    bool searchEditing_ = false;
    bool controllerTabLabelReady_ = false;
    float keyboardBarTop_ = 588;
    unsigned keyboardRequest_ = 0;
    std::string searchQuery_, searchComposition_;
    std::vector<int> searchMatches_;
    int volume_ = 10;
    double optionPopupAt_ = 0;
    double settingsLeavingAt_ = -1;
    int selectedCrew_ = 0;
    int crewPreview_ = 0;
    int crewOffset_ = 0;
    int crewOnMain_ = 0;
    bool crewSwitchPending_ = false;
    bool startAfterSubscreen_ = false;
    bool displayReady_ = true;
    double displayWaitingAt_ = -1;
    int crewButton_ = -1;
    int roomFocus_ = 1;
    int valkyrieVolume_ = 12;
    int modeIndex_ = NormalMode, creditFocus_ = 0, credits_ = 0, paseli_ = DefaultPaseli;
    bool modeLabelsReady_ = false;
    bool nextAreaLabelsReady_ = false;
    bool matchingCardReady_ = false;
    StartType sessionType_ = StartType::Light;
    int track_ = 1;
    double practiceUntil_ = 0, exitElapsed_ = 0, exitDecidedAt_ = 0;
    PinPhase pinPhase_ = PinPhase::Create;
    EntryStep entryStep_ = EntryStep::Pin;
    std::optional<EntryStep> entryNext_;
    std::optional<std::future<bool>> pinVerification_;
    double pinVerificationAt_ = 0;
    double entryLoadingVoiceAt_ = -1;
    double entryAt_ = 0, entryOutAt_ = -1, paymentAcceptedAt_ = -1;
    double paymentReceiptAt_ = -1, paymentReceiptOutAt_ = -1;
    int paymentReceiptPrice_ = 0;
    int pinCountdown_ = 0;
    bool paymentCanceled_ = false;
    bool cautionOpened_ = false, cautionClosing_ = false;
    int entryFocus_ = 0, nameIndex_ = 0;
    bool entryNeedsName_ = false, entryAuthenticated_ = false;
    bool entryProfileReady_ = false;
    std::string playerName_ = "LOCAL PLAYER", draftName_;
    int pinFocus_ = 0, exitFocus_ = 1;
    std::string cardId_, pin_, pendingPin_, pinError_;
    int songIndex_ = 0, chartIndex_ = 0, wheelIndex_ = 0;
    int sort_ = 0;
    float songScrollIndex_ = -1.f, songScrollFrame_ = -1.f;
    double modeStarted_ = 0, roomStarted_ = 0, roomDepartureAt_ = -1;
    double crewLeavingAt_ = -1, crewChangedAt_ = 0, crewButtonAt_ = -1;
    bool matchingGo_ = false, matchingSound_ = false, characterView_ = false;
    bool titleCallPending_ = false, alternateTitleCall_ = false;
    enum class MenuWarmup { Pending, Parsing, Queued };
    MenuWarmup menuWarmup_ = MenuWarmup::Pending;
    bool continuing_ = false;
    Page page_ = Page::Title, settingsReturn_ = Page::Songs, exitReturn_ = Page::Room;
    Wheel wheel_ = Wheel::Songs;
    std::optional<Transition> transition_;
    std::optional<Selection> pendingSelection_;
    std::string activeMusic_, message_;
    double now_ = 0, entered_ = 0, previewAt_ = -1, messageUntil_ = 0, coveredAt_ = 0;
    // Destroy the startup future before the catalog fields its worker writes.
    std::optional<Startup> startup_;
};
}
