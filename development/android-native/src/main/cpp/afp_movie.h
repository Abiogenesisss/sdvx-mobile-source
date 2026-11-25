#pragma once

#include "app.h"

#include <nlohmann/json_fwd.hpp>

namespace sdvx {

class AfpMovie {
public:
    AfpMovie(std::string_view folder, std::string_view movie);
    ~AfpMovie();
    AfpMovie(AfpMovie&&) noexcept;
    AfpMovie& operator=(AfpMovie&&) noexcept;
    AfpMovie fork() const;

    void start(std::string_view label, double time);
    void start(size_t frame, double time);
    void stop();
    bool started() const;
    void holdRoot();
    void configure(const nlohmann::json& options);
    void setRate(double rate, double time);
    void cue(std::string_view path, std::string_view label, double time);
    void cue(std::string_view path, std::string_view label, double time, bool rewindBackward);
    void applyCues();
    void parameter(std::string_view path, std::string_view fromLabel, std::string_view toLabel,
                   double start, double end, double from, double to);
    void advance(double time);
    double fps() const;
    Vec2 size() const;
    std::optional<size_t> label(std::string_view name) const;
    Render::AfpAnchor anchor(std::string_view instance) const;
    size_t frames() const;
    void queueTextures(Render& render, bool stage, bool deferred = true) const;
    void draw(Render& render, Rect bounds, const Bindings& bindings,
              const std::array<Render::Projected, 3>* plane = nullptr, float opacity = 1) const;

private:
    struct Impl;
    explicit AfpMovie(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}
