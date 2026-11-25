#pragma once

#include "app.h"

namespace sdvx {

struct LanePreview {
    void update(double positionMs);
    void press(int button, double now, double inputAt, const PlayOptions& options);
    void draw(Render& render, double now, const PlayOptions& options,
              const std::array<bool, ButtonCount>& held) const;
    struct Hit { int beat = -1, judgment = 0; double at = -1, offset = 0; };
    std::array<Hit, 4> hits;
    double timeMs = 0, previousPosition = 0;
    int cycles = 0;
};

class Stage {
public:
    struct SongData;
    static std::shared_ptr<const SongData> prepareSong(const Selection& selection);
    static void queueGraphics(Render& render, const Selection& selection);
    static void queuePreparedImages(Render& render, const SongData& song);
    Stage(Render& render, Audio& audio, Selection selection, PlayOptions options, PlayerCard player,
          int previousBest, std::shared_ptr<const SongData> song, bool deferTextures = false);
    ~Stage();
    void update(double nowMs);
    void draw(double nowMs);
    bool prepareGraphics();
    void input(const GameInput& event);
    void setTouchControls(bool enabled, double now);
    void releaseGraphics();
    void restoreGraphics();
    std::optional<Result> takeResult();
    const PlayOptions& options() const;
    void processAudio(float* interleavedStereo, int frames, int64_t firstFrame);

private:
    void prepareAudio();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sdvx
