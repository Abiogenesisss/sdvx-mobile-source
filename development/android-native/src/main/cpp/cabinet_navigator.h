#pragma once

#include "app.h"
#include "cabinet_crew_model.h"
#include <deque>

namespace sdvx {

enum class NavigatorCue {
    Silent, Hidden, TitleCallFirst, TitleCallSecond, PinPrompt, PinChecking, PinRejected, PinAccepted, EntryLoading,
    Introduction, Room, RoomReturn, Depart,
    CrewSelect, CrewConfirmed, AppealCardSelect, AppealCardSaved, Leave,
    Generator, Valkyrie, Mode, Payment, FirstTrack, SecondTrack, FinalTrack, ExtraTrack, SongPrompt,
    PremiumStart, Matching, ForceIncrease, ForceLevelUp, ForceClassUp,
    SkillLevelUp, SkillFailed, Total, Continue, ContinueYes, ContinueNo, Saved, FarewellAlternate, Farewell
};

class Navigator {
public:
    static std::string asset(int crew, std::string_view file);

    Navigator(Render& render, Audio& audio);
    void play(NavigatorCue cue, int crew, double now);
    void enqueue(NavigatorCue cue, double now);
    void result(const Result& result, bool changedLamp, int crew, double now, double gradePresentationMs = 0);
    void update(double now);
    bool acting() const;
    bool speaking() const { return audio_.narrating(); }
    bool readyForVoice() const { return nextVoices_.empty() && !speaking(); }
    bool celebratingResult() const { return resultCommentStarted_ && speaking(); }
    void draw(CrewScene scene);
    bool prepareModel(int crew) {
        model_.select(crew, false, true);
        return model_.prepareGraphics();
    }
    void releaseGraphics() { model_.releaseGraphics(); }
    void restoreGraphics() { model_.restoreGraphics(); }

private:
    enum class VoiceStart { AfterPlayback, Timed, ResultComment };
    struct Voice {
        std::string_view file;
        double delayMs = 0;
        VoiceStart start = VoiceStart::AfterPlayback;
        bool system = false;
    };

    void motion(std::string_view clip, std::string_view next, double now);
    void dispatchVoice(double now);
    bool motionFinished() const;
    int voiceCrew(bool system) const;

    Render& render_;
    Audio& audio_;
    CrewModel model_;
    int crew_ = 0;
    int systemCrew_;
    std::string_view clip_;
    std::string_view next_;
    std::deque<Voice> nextVoices_;
    double updatedAt_ = 0, nextVoiceAt_ = -1;
    double modeEntranceAt_ = -1;
    double resultCommentAt_ = 0;
    bool resultCommentStarted_ = false;
    double motionTime_ = 0, motionDuration_ = 0;
    CrewScene scene_ = CrewScene::Room;
    bool loop_ = false;
};

}
