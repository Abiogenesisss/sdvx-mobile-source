#include "cabinet_navigator.h"
#include "cabinet_crew_catalog.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>

namespace sdvx {
namespace {
struct CrewAfp {
    std::string_view folder, animation, clip;
    Rect bounds;
};

CrewAfp legacyMotion(const CrewInfo& crew, CrewScene scene, std::string_view motion) {
    if (const auto found = crew.afpMotions.find(std::string(motion)); found != crew.afpMotions.end())
        return {crew.roomAfp, found->second[0], found->second[1], {0, 0, Width, Height}};
    if (scene == CrewScene::Matching && !crew.matchingAfp.empty()) {
        const auto animation = motion == "In" ? "mach_in" : motion == "Out" ? "mach_go" : "mach_neu";
        return {crew.matchingAfp, animation, motion == "Idle" ? "neu_loop" : "0", {0, 240, Width, Width}};
    }
    const auto clip = motion == "In" ? "in" : motion == "Out" ? "out" :
        motion == "Ok" || motion == "R_Good" || motion == "R_Verygood" ? "kettei" : "neu_loop";
    return {crew.roomAfp, "chara_anime", clip, {0, 0, Width, Height}};
}

struct Behavior {
    std::string_view voice;
    std::string_view motion = {};
    std::string_view next = {};
    bool system = false;
};

constexpr Behavior behavior(NavigatorCue cue) {
    switch (cue) {
    using enum NavigatorCue;
    case Silent: return {};
    case Hidden: return {};
    case TitleCallFirst: return {"title02.wav", {}, {}, true};
    case TitleCallSecond: return {"title03.wav", {}, {}, true};
    case PinPrompt: return {"entry14.wav", {}, {}, true};
    case PinChecking: return {"entry11.wav", {}, {}, true};
    case PinRejected: return {"entry07.wav", {}, {}, true};
    case PinAccepted: return {"entry12.wav", {}, {}, true};
    case EntryLoading: return {"entry13.wav", {}, {}, true};
    case Introduction: return {"entry02.wav", "In", "Idle"};
    case Room: return {"cus01.wav"};
    case RoomReturn: return {{}, "In", "Idle"};
    case Depart: return {"mode05.wav", "Out", {}};
    case CrewSelect: return {"cus02.wav"};
    case CrewConfirmed: return {"cus03.wav", "In", "Idle"};
    case AppealCardSelect: return {"cus04.wav"};
    case AppealCardSaved: return {"cus05.wav"};
    case Leave: return {{}, "Out", {}};
    case Generator: return {"vg02.wav"};
    case Valkyrie: return {"vg01.wav"};
    case Mode: return {"mode01.wav"};
    case Payment: return {"mode02.wav"};
    case FirstTrack: return {"music10.wav"};
    case SecondTrack: return {"music11.wav"};
    case FinalTrack: return {"music13.wav"};
    case ExtraTrack: return {"music14.wav"};
    case SongPrompt: return {"music06.wav"};
    case PremiumStart: return {"music15.wav"};
    case Matching: return {"match01.wav", "In", "Idle"};
    case ForceIncrease: return {"result12.wav"};
    case ForceLevelUp: return {"result13.wav"};
    case ForceClassUp: return {"result14.wav"};
    case SkillLevelUp: return {"result21.wav"};
    case SkillFailed: return {"result22.wav"};
    case Total: return {"total01.wav"};
    case Continue: return {"end01.wav"};
    case ContinueYes: return {"end03.wav"};
    case ContinueNo: return {"end02.wav"};
    case Saved: return {"end04.wav"};
    case FarewellAlternate: return {"end05.wav"};
    case Farewell: return {"end06.wav"};
    }
    return {};
}
}

Navigator::Navigator(Render& render, Audio& audio)
    : render_(render), audio_(audio), systemCrew_(SDL_rand(2)) {}

int Navigator::voiceCrew(bool system) const {
    // Silent crews still use a built-in narrator before MY ROOM. Keep that
    // narrator consistent across the title and queued PIN prompts.
    return system && crewInfo(crew_).voice.empty() ? systemCrew_ : crew_;
}

std::string Navigator::asset(int crew, std::string_view file) {
    if (file == "icon.png") return crewInfo(crew).icon;
    const auto* voice = crewVoice(crew, file);
    return voice ? voice->path : std::string{};
}

void Navigator::motion(std::string_view clip, std::string_view next, double now) {
    clip_ = clip;
    next_ = next;
    updatedAt_ = now;
    loop_ = clip == "Idle" || clip.ends_with("_Loop");
    motionTime_ = motionDuration_ = 0;
    if (!clip_.empty()) {
        model_.select(crew_);
        model_.start(clip_, loop_ ? 1 : 2);
        const auto& crew = crewInfo(crew_);
        if (crew.actors.empty()) {
            const auto afp = legacyMotion(crew, scene_, clip);
            if (!afp.folder.empty()) motionDuration_ = render_.afpDuration(afp.folder, afp.animation, afp.clip);
        }
        SDL_Log("SDVX Navigator motion %d:%d/%.*s", crew.id, crew.style, int(clip_.size()), clip_.data());
    }
}

bool Navigator::motionFinished() const { return model_.finished() && motionTime_ >= motionDuration_; }
bool Navigator::acting() const { return !clip_.empty() && !loop_ && !motionFinished(); }

void Navigator::play(NavigatorCue cue, int crew, double now) {
    const auto action = behavior(cue);
    nextVoices_.clear();
    nextVoiceAt_ = -1;
    modeEntranceAt_ = -1;
    resultCommentStarted_ = false;
    if (!action.voice.empty()) {
        crew_ = crew;
        if (const auto* voice = crewVoice(voiceCrew(action.system), action.voice)) audio_.announce(voice->path, true, voice->gain);
        else audio_.stopNarration();
    } else {
        audio_.stopNarration();
    }
    if (cue == NavigatorCue::Mode) {
        // 18025611d gives mode02 a 500 ms delay after mode01 completes.
        nextVoices_.push_back({"mode02.wav", 500});
        // The MODE AFP starts first. The original 0x1804e53b1 starts the room
        // actor later, about 0.68 s after mode01 in the GRACE reference.
        crew_ = crew;
        modeEntranceAt_ = now + 680;
    }
    if (cue == NavigatorCue::Hidden) {
        motion({}, {}, now);
    } else if (!action.motion.empty()) {
        crew_ = crew;
        if (cue == NavigatorCue::Matching) scene_ = CrewScene::Matching;
        else if (action.motion == "In") scene_ = CrewScene::Room;
        if (cue == NavigatorCue::Introduction || cue == NavigatorCue::RoomReturn || cue == NavigatorCue::Matching)
            model_.select(crew, true);
        motion(action.motion, action.next, now);
    }
}

void Navigator::enqueue(NavigatorCue cue, double now) {
    const auto action = behavior(cue);
    nextVoices_.push_back({action.voice, 0, VoiceStart::AfterPlayback, action.system});
    dispatchVoice(now);
}

void Navigator::result(const Result& result, bool changedLamp, int crew, double now, double gradePresentationMs) {
    modeEntranceAt_ = -1;
    resultCommentAt_ = now + gradePresentationMs;
    resultCommentStarted_ = false;
    constexpr std::array<std::string_view, 10> grades{
        "s", "aaa_plus", "aaa", "aa_plus", "aa", "a_plus", "a", "b", "c", "d"
    };
    constexpr std::array<std::string_view, grades.size()> gradeVoices{
        "result02.wav", "result03.wav", "result04.wav", "result05.wav", "result06.wav",
        "result07.wav", "result08.wav", "result09.wav", "result10.wav", "result11.wav"
    };
    crew_ = crew;
    scene_ = CrewScene::Result;
    // 1803b1300 initializes the result component before dispatching its grade
    // reaction. Do not inherit the departed menu model's saved parameters.
    model_.select(crew, true);
    audio_.stopNarration();
    const auto grade = static_cast<size_t>(std::find(grades.begin(), grades.end(), result.grade) - grades.begin());
    // The cabinet selects the motion by grade and the spoken reaction by clear lamp.
    const bool veryGood = grade < 3;
    const auto reaction = veryGood ? "R_Verygood" : grade < 7 ? "R_Good" : "R_Bad";
    const bool perfect = result.outcome == "puc" || result.outcome == "spuc";
    const auto voice = changedLamp && perfect ? "result15.wav"
        : changedLamp && result.outcome == "uc" ? "result16.wav"
        : result.outcome == "crash" ? (SDL_rand(2) ? "result20.wav" : "result19.wav")
        : (SDL_rand(2) ? "result18.wav" : "result17.wav");
    // 1803ad6d0 schedules the grade from the intro's start and the comment from the grade's end.
    nextVoices_ = {{"result01.wav"},
                   {gradeVoices[std::min(grade, grades.size() - 1)], grade == 0 ? 2500. : 1500., VoiceStart::Timed},
                   {voice, 0, VoiceStart::ResultComment}};
    nextVoiceAt_ = now + 200;
    // The original queue repeats only its explicitly marked last entry.
    // Ordinary result reactions keep their final pose, with Cubism still updating.
    const std::string_view tail = veryGood ? "R_Verygood_Loop" : grade < 7 ? "R_Good_Loop" : "R_Bad_Loop";
    const bool hasTail = model_.hasMotion(tail) || crewInfo(crew).afpMotions.contains(std::string(tail));
    motion(reaction, hasTail ? tail : std::string_view{}, now);
}

void Navigator::update(double now) {
    if (modeEntranceAt_ >= 0 && now >= modeEntranceAt_) {
        modeEntranceAt_ = -1;
        scene_ = CrewScene::Room;
        model_.select(crew_, true);
        motion("In", "Idle", now);
    }
    if (!clip_.empty()) {
        const double delta = std::max(0., now - updatedAt_);
        motionTime_ += delta;
        model_.update(delta / 1000);
        if (motionFinished()) {
            if (!next_.empty()) motion(next_, {}, now);
            else if (loop_ && motionDuration_ == 0) model_.start(clip_, 1);
            else if (clip_ == "Out") clip_ = {};
        }
    }
    updatedAt_ = now;
    dispatchVoice(now);
}

void Navigator::dispatchVoice(double now) {
    if (nextVoices_.empty()) return;
    const auto& next = nextVoices_.front();
    if (next.start == VoiceStart::ResultComment && now < resultCommentAt_) return;
    if (nextVoiceAt_ < 0) {
        if (audio_.narrating()) return;
        nextVoiceAt_ = now + next.delayMs;
    }
    if (now < nextVoiceAt_) return;

    const auto* voice = crewVoice(voiceCrew(next.system), next.file);
    if (next.start == VoiceStart::ResultComment) resultCommentStarted_ = true;
    nextVoices_.pop_front();
    nextVoiceAt_ = !nextVoices_.empty() && nextVoices_.front().start == VoiceStart::Timed
        ? now + nextVoices_.front().delayMs : -1;
    if (voice) audio_.announce(voice->path, false, voice->gain);
}

void Navigator::draw(CrewScene scene) {
    if (!clip_.empty()) {
        const auto& crew = crewInfo(crew_);
        if (crew.actors.empty()) {
            const auto afp = legacyMotion(crew, scene, clip_);
            if (!afp.folder.empty()) {
                render_.afp(afp.folder, afp.animation, afp.clip, motionTime_, loop_, afp.bounds);
                return;
            }
            if (!crew.portrait.empty()) {
                const auto size = render_.imageSize(crew.portrait);
                const float scale = std::min(Width / size.x, Height / size.y);
                render_.image(crew.portrait, {(Width - size.x * scale) / 2, 0, size.x * scale, size.y * scale});
                return;
            }
        }
        render_.flush();
        model_.draw(scene);
    }
}

}
