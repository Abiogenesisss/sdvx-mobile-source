#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace sdvx {

inline constexpr int stageSampleRate = 48000;
inline constexpr double laserNoticeLeadMs = 650;
inline constexpr int judgmentCritical = 5;
inline constexpr int judgmentMiss = 10;

// 2026101300: 180472880 adds the judgment setting; the draw-clock
// callback subtracts the display setting. Both settings are milliseconds.
inline double stageJudgeTime(double audioMs, double settingMs) { return audioMs - 12 - 1000.0/480 + settingMs; }
inline double stageDrawTime(double audioMs, double settingMs) { return audioMs + 2 + 1000.0/24 - settingMs; }

enum class ClearType { Complete, Crash, Ultimate, Perfect, SPerfect };

struct StageScore {
    int critical = 0;
    int near = 0;
    int errors = 0;
    int chain = 0;
    int maxChain = 0;
    int total = 0;
    int maxEx = 0;
    double gauge = 0;
    bool excessive = false, ultra = false;
};

struct StageNote {
    int button;
    double start, end;
    int preset, sample, cycle;
    float mix;
    bool longNote, headJudged = false, active = false, judged = false;
    double holdGroupStart = 0;
    int fxLeadFrames = 0;
};

struct LaserPoint {
    double time;
    float position;
    int tick;
    float range;
    int filter;
    bool phraseHead;
    int tickStep = 0;
    bool startsPhrase = false;
    double sectionStart = 0;
};

struct StageLaser {
    int side;
    LaserPoint start, end;
    double cursorEnd;
    bool pendingSnap = false;
    std::optional<double> turnAssistAt = std::nullopt;
    bool slam() const { return start.tick == end.tick && start.position != end.position; }
    float range() const;
    int direction() const { return (end.position > start.position) - (end.position < start.position); }
    float positionAt(double tick) const;
};

struct StageCue {
    std::string name;
    double time, end;
    std::vector<std::string> parameters;
    int mode = 0;
    std::optional<int> nemsys = std::nullopt;
    struct PostTarget { unsigned layer, index; };
    std::optional<PostTarget> post = std::nullopt;
};

struct AutoplayFeedback {
    std::vector<const StageNote*> samples;
    std::vector<const StageLaser*> slams;
};

struct ButtonPress {
    int judgment = 0;
    int sample = 0;
    std::optional<double> offsetMs{};
};

struct LaserEffect {
    double time = -1;
    float position = 0;
    int direction = 0;
};

class StageChart {
public:
    static constexpr float RotaryUnitsPerLane = 768;
    struct TiltTarget { float value, speed; };
    StageChart(const nlohmann::json& source, bool mirror, bool excessive, bool ars,
               bool autoPlay = false, double permissiveRate = -1, int randomMode = 0, uint32_t seed = 1,
               bool ultra = false);

    void advance(double time);
    ButtonPress press(int button, double time);
    void release(int button, double time);
    const StageLaser* rotate(int side, float delta, double time);
    void clearInputs(double time);
    void processAudio(float* samples, int frames, int64_t firstFrame,
                      std::span<const int16_t> dryPcm);

    float cursor(int side) const { return knobs_[side].position; }
    std::optional<float> cursorForDraw(int side, double time) const;
    bool tracking(int side) const { return knobs_[side].active; }
    TiltTarget tiltTarget(double time) const;
    const LaserEffect& laserCorner(int side) const { return knobs_[side].corner; }
    const LaserEffect& laserExit(int side) const { return knobs_[side].exit; }
    int laserMaterial(const StageLaser& laser) const;
    bool holding(int button) const;
    int feedback(int button) const { return feedback_[button]; }
    unsigned feedbackSequence(int button) const { return feedbackSequence_[button]; }
    const AutoplayFeedback& autoplayFeedback() const { return autoFeedback_; }
    bool failedRate() const { return failedRate_; }
    void forceCrash() { stats_.gauge = 0; failedRate_ = true; }
    const StageScore& stats() const { return stats_; }
    int score() const;
    int exScore() const;
    ClearType clearType(bool showS, bool forcedCrash) const;
    const std::array<std::array<int, 11>, 3>& judgments() const { return results_; }
    double timeAt(double tick) const;
    double tickAt(double time) const;
    double bpmAt(double time) const;
    double scrollBpm(bool constantSpeed) const;
    double scrollPosition(double time, bool constantSpeed) const;
    double scrollTime(double position, bool constantSpeed) const;
    std::vector<double> barLines;

    double endMs;
    std::vector<StageNote> notes;
    std::vector<StageLaser> lasers;
    std::vector<std::pair<int, double>> laserStarts;
    std::vector<StageCue> cues;

private:
    struct Tempo { int tick; double time, bpm; };
    struct ScrollSection { double time, position, unitsPerMs; };
    struct TiltKey { double time; float value; };
    enum Window { Chip, FxChip, HoldWide, HoldNarrow, LaserTrack, LaserSlam };
    enum Kind { ChipObject, HoldObject, LaserObject };
    enum Check { Head, Hold, Tail, Track, Slam };
    struct Boundary { float seconds; int result; };
    struct Checkpoint {
        Check type;
        size_t object;
        double time, deadline;
        bool scores = false, resolved = false, contacted = false;
    };
    struct Button {
        std::vector<size_t> chips, holds;
        std::deque<std::pair<double, double>> presses;
        size_t nextChip = 0, nextHold = 0;
        bool down = false;
        double pressedAt = 0;
    };
    struct FilterHistory { float x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    struct KnobAudio {
        uint32_t preset = 0;
        int position = 0;
        bool authored = false;
    };
    struct Knob {
        std::vector<size_t> lasers, tracks, slams;
        size_t nextLaser = 0, nextTrack = 0, nextSlam = 0;
        float position = 0;
        bool entered = false, active = false;
        LaserEffect corner, exit;
        int direction = 0;
        int heldDirection = 0;
        double directionUntil = 0;
    };
    using Preset = std::array<float, 8>;
    struct TabAssignment { int parameter = 0; float from = 0, to = 0; };

    const Tempo& tempoAt(double time) const;
    int resultAt(Window table, double input, double object) const;
    Window chipWindow(const StageNote& note) const;
    StageNote* currentHold(Button& button);
    const StageLaser* currentLaser(const Knob& knob) const;
    KnobAudio advanceKnob(int side, double from, double to);
    void markLaserContact(Knob& knob, double from, double to, bool following);
    int judge(Checkpoint& checkpoint);
    void judgeChip(StageNote& note, int result);
    void record(int result, Kind kind, bool slam = false);
    void setFeedback(int button, int result);
    void publishHold(int button, const Button& state, const StageNote* hold);
    KnobAudio tabControl(int side, const Knob& knob, const StageLaser* laser, double time) const;
    void publishTabs(double time);
    void releaseButton(int index, double time);

    std::vector<Tempo> tempos_;
    std::array<std::vector<ScrollSection>, 2> scroll_;
    std::vector<std::vector<TiltKey>> tiltCurves_;
    double representativeBpm_ = 0;
    int ticksPerQuarter_;
    std::array<std::vector<Boundary>, 6> windows_;
    std::vector<Preset> buttonPresets_, tabPresets_;
    std::vector<std::array<TabAssignment, 2>> tabAssignments_;
    std::array<Button, 6> buttons_;
    std::array<Knob, 2> knobs_;
    std::vector<Checkpoint> checks_;
    std::array<std::array<int, 11>, 3> results_{};
    std::array<int, 6> feedback_{};
    std::array<unsigned, 6> feedbackSequence_{};
    std::array<std::atomic<int>, 2> holds_{-1, -1};
    std::atomic<uint64_t> tabControls_{};
    std::array<KnobAudio, 2> knobAudio_{};
    std::deque<std::pair<double, int>> pendingEq_;
    int queuedEq_ = 0, liveEq_ = 0;
    std::array<FilterHistory, 2> liveEqHistory_{};
    struct TabHistory {
        std::array<FilterHistory, 2> filter{};
        std::array<float, 2> held{};
        int blockFrame = 0;
        int reduction = 1;
    };
    std::vector<TabHistory> tabHistory_;
    std::array<std::array<FilterHistory, 2>, 2> wobbleHistory_{};
    std::array<int, 2> activeWobbleNotes_{-1, -1};
    struct FlangerHistory {
        // The original delay is at most 6 ms, or 288 frames at 48 kHz.
        static constexpr int frames = 512;
        std::array<std::array<std::array<float, 2>, frames>, 5> input{};
        int64_t first = 0, next = 0;
        int note = -1;
    };
    std::array<FlangerHistory, 2> flangerHistory_{};
    std::array<uint32_t, 2> activeTabIds_{};
    StageScore stats_;
    int totalChips_ = 0;
    size_t nextCheck_ = 0;
    double advancedTo_ = -1e10;
    double gaugeGain_ = 0;
    double ultraGain_ = 0;
    double effectiveGauge_ = 0, excessiveGauge_ = 1;
    bool ars_, autoPlay_, permissive_, failedRate_ = false;
    AutoplayFeedback autoFeedback_;
};

} // namespace sdvx
