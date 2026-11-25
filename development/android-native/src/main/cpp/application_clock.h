#pragma once

namespace sdvx {

class ApplicationClock {
public:
    enum class Activity { Active, Background, Resuming };

    Activity activity() const { return activity_; }
    double eventTime(double wallMs) const { return wallMs - suspendedMs_; }
    double now(double wallMs) const {
        return eventTime(activity_ == Activity::Active ? wallMs : suspendedAt_);
    }

    void enterBackground(double wallMs) {
        if (activity_ == Activity::Active) suspendedAt_ = wallMs;
        activity_ = Activity::Background;
    }

    bool enterForeground() {
        if (activity_ != Activity::Background) return false;
        activity_ = Activity::Resuming;
        return true;
    }

    void resume(double wallMs) {
        suspendedMs_ += wallMs - suspendedAt_;
        activity_ = Activity::Active;
    }

private:
    Activity activity_ = Activity::Active;
    double suspendedAt_ = 0;
    double suspendedMs_ = 0;
};

}
