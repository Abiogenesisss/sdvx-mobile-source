#pragma once

#include "app.h"

#include <memory>
#include <span>

namespace sdvx {

struct StageCue;

struct StagePostPass {
    int firstPriority;
    int endPriority;
    int outputPriority;
    std::vector<int> effects;
    enum class Output { Filtered, Movie, Inversion };
    Output output;
    int defaultMode = 0;
};
inline constexpr std::array<std::string_view, 2> StageCaptureTargets{"runtime/stage-range-0", "runtime/stage-range-1"};
inline constexpr std::array<std::string_view, 2> StageFilterTargets{"runtime/stage-range-filter-0", "runtime/stage-range-filter-1"};

std::string stageBackgroundFolder(const Selection& selection);
std::string commonStageBackground(int number);

class StageScene {
public:
    struct Source;
    static std::shared_ptr<const Source> load(const Selection& selection);
    static void queueSourceImages(Render& render, const Source& source);
    StageScene(Render& render, const Selection& selection, const Source& source, bool deferTextures);
    ~StageScene();

    void cue(const StageCue& cue);
    void setGauge(double time, bool clear);
    void update(double time, double now, double bpm);
    void queueInitial(std::span<const StageCue> cues);
    void queueGraphics(std::span<const StageCue> cues);
    bool prepareGraphics();
    void releaseGraphics();
    void restoreGraphics();

    std::span<const StagePostPass> postPasses() const;
    void drawAtPriorities(double time, std::optional<Vec2> center, float pitch, float autoTilt, float roll,
                          int first, int end);
    std::span<const int> drawPriorities() const;
    bool postMovieActive(size_t index) const;
    void drawPostInversion(size_t index, std::string_view source);
    void drawPostMovie(std::string_view source,
                       const std::function<bool(size_t, std::string_view, std::string_view)>& filter);
    bool extendedNoteDepth() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
