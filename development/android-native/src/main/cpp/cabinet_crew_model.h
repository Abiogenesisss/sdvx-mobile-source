#pragma once

#include "app.h"

namespace sdvx {
enum class CrewScene { Room, Matching, Result };

// One current Cubism model owns its saved parameters, motion queue and physics.
// Navigator chooses behaviors; drawing never starts or advances a motion.
class CrewModel {
public:
    CrewModel();
    ~CrewModel();
    static void prepareShaders();
    void select(int crew, bool reset = false, bool deferTextures = false);
    bool prepareGraphics();
    void start(std::string_view group, int priority);
    void update(float seconds);
    bool finished() const;
    bool hasMotion(std::string_view group) const;
    void draw(CrewScene scene);
    void releaseGraphics();
    void restoreGraphics();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class StageModel {
public:
    StageModel(const std::string& path, std::array<float, 4> placement, bool deferTextures = false);
    ~StageModel();
    void setPlacement(std::array<float, 4> placement);
    void play(std::string_view motion, std::string_view loop, double time);
    void update(double time);
    void draw(Render& render);
    bool prepareGraphics();
    void releaseGraphics();
    void restoreGraphics();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
