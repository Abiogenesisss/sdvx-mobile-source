#pragma once

#include "app.h"

namespace sdvx {
class AfpLibrary {
public:
    AfpLibrary();
    ~AfpLibrary();
    AfpLibrary(AfpLibrary&&) noexcept;
    AfpLibrary& operator=(AfpLibrary&&) noexcept;
    bool draw(Render& render, std::string_view folder, std::string_view animation, std::string_view clip,
              double ms, bool loop, Rect bounds, const Bindings& bindings,
              const std::array<Render::Projected, 3>* plane = nullptr, float opacity = 1);
    double duration(std::string_view folder, std::string_view animation, std::string_view clip);
    Vec2 size(std::string_view folder, std::string_view animation, std::string_view clip);
    Render::AfpAnchor anchor(std::string_view folder, std::string_view animation, std::string_view clip,
                            std::string_view instance);
    void prefetch(Render&, std::string_view folder, std::string_view animation, std::string_view clip,
                  AfpTextures textures, bool deferred = false, bool stage = false);
    void queueData(std::span<const std::string_view> folders, std::string_view extra = {});
    bool dataReady();
    void releaseStageData();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
