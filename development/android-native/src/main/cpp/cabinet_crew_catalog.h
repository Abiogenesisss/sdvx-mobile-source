#pragma once

#include "app.h"

namespace sdvx {

struct CrewPlacement {
    float x = 0, y = 0, scaleX = 1, scaleY = 1;
};

struct CrewActor {
    std::string model;
    std::array<CrewPlacement, 3> placements;
    std::unordered_map<std::string, std::string> motions;
};

struct CrewInfo {
    int id, style, order;
    std::string icon, preview, nameImage, name, portrait, roomAfp, matchingAfp, voice;
    std::vector<int> actors;
    bool installed = true;
    std::unordered_map<std::string, std::array<std::string, 2>> afpMotions;
};

struct CrewFamily {
    int id;
    std::vector<int> styles;
};

struct CrewVoice {
    std::string path;
    float gain;
};

const CrewInfo& crewInfo(int index);
const CrewActor& crewActor(int index);
const std::vector<CrewFamily>& crewFamilies();
const CrewVoice* crewVoice(int crew, std::string_view cue);

} // namespace sdvx
