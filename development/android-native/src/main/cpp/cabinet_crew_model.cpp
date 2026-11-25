#include "cabinet_crew_model.h"
#include "cabinet_crew_catalog.h"

#include <GLES3/gl32.h>
#include <SDL3/SDL_log.h>
#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <ICubismAllocator.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismUserModel.hpp>
#include <Motion/CubismMotion.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Rendering/OpenGL/CubismShader_OpenGLES2.hpp>
#include <Rendering/CubismClippingManager.hpp>
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <set>
#include <stdexcept>

namespace sdvx {
namespace {
namespace Csm = Live2D::Cubism::Framework;
using GlRenderer = Csm::Rendering::CubismRenderer_OpenGLES2;

class Allocator final : public Csm::ICubismAllocator {
    void* Allocate(Csm::csmSizeType bytes) override { return std::malloc(bytes); }
    void Deallocate(void* pointer) override { std::free(pointer); }
    void* AllocateAligned(Csm::csmSizeType bytes, Csm::csmUint32 alignment) override {
        void* pointer = nullptr;
        return posix_memalign(&pointer, alignment, bytes) == 0 ? pointer : nullptr;
    }
    void DeallocateAligned(void* pointer) override { std::free(pointer); }
};

class CubismRuntime {
public:
    static std::shared_ptr<CubismRuntime> acquire() {
        static std::weak_ptr<CubismRuntime> current;
        auto runtime = current.lock();
        if (!runtime) current = runtime = std::make_shared<CubismRuntime>();
        return runtime;
    }

    CubismRuntime() {
        options_.LogFunction = [](const char* message) { SDL_Log("Cubism: %s", message); };
        options_.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Warning;
        options_.LoadFileFunction = [](const std::string path, Csm::csmSizeInt* size) {
            const auto data = readAsset("crew/shaders/" + path);
            *size = data.size();
            auto* buffer = new Csm::csmByte[data.size()];
            std::memcpy(buffer, data.data(), data.size());
            return buffer;
        };
        options_.ReleaseBytesFunction = [](Csm::csmByte* bytes) { delete[] bytes; };
        Csm::CubismFramework::StartUp(&allocator_, &options_);
        Csm::CubismFramework::Initialize();
    }

    ~CubismRuntime() {
        Csm::CubismFramework::Dispose();
        Csm::CubismFramework::CleanUp();
    }

private:
    Allocator allocator_;
    Csm::CubismFramework::Option options_{};
};

struct ModelTexture {
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{nullptr, stbi_image_free};
    int width = 0, height = 0;
};

ModelTexture decodeModelTexture(const std::string& path) {
    const auto bytes = readAsset(path);
    ModelTexture image;
    image.pixels.reset(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                            bytes.size(), &image.width, &image.height, nullptr, 4));
    if (!image.pixels) throw std::runtime_error("Invalid Cubism texture: " + path +
        " (" + std::to_string(bytes.size()) + " bytes; " + stbi_failure_reason() + ")");
    auto* pixels = image.pixels.get();
    for (int pixel = 0; pixel < image.width * image.height; ++pixel)
        for (int channel = 0; channel < 3; ++channel)
            pixels[4 * pixel + channel] = (unsigned(pixels[4 * pixel + channel]) * pixels[4 * pixel + 3] + 127) / 255;
    return image;
}

class Model final : public Csm::CubismUserModel {
public:
    explicit Model(const std::string& path, bool deferTextures = false, bool stage = false) {
        const auto root = path.substr(0, path.rfind('/') + 1);
        const auto json = readAsset(path);
        Csm::CubismModelSettingJson settings(reinterpret_cast<const Csm::csmByte*>(json.data()), json.size());
        const auto moc = readAsset(root + settings.GetModelFileName());
        LoadModel(reinterpret_cast<const Csm::csmByte*>(moc.data()), moc.size(), true);
        if (!_model) throw std::runtime_error("Invalid Cubism model: " + path);
        {
            std::set<std::vector<Csm::csmInt32>> masks;
            const auto counts = _model->GetDrawableMaskCounts();
            const auto drawables = _model->GetDrawableMasks();
            for (int i = 0; i < _model->GetDrawableCount(); ++i) {
                if (!counts[i]) continue;
                std::vector<Csm::csmInt32> mask(drawables[i], drawables[i] + counts[i]);
                std::ranges::sort(mask);
                masks.insert(std::move(mask));
            }
            // Menu crews can exceed one target too (e.g. 44 groups). Cubism
            // uses fewer slots per target when multiple targets are allocated.
            if (masks.size() > Csm::Rendering::ClippingMaskMaxCountOnDefault) {
                constexpr auto capacity = Csm::Rendering::ClippingMaskMaxCountOnMultiRenderTexture;
                maskBuffers_ = (masks.size() + capacity - 1) / capacity;
            }
        }
        if (const auto* physics = settings.GetPhysicsFileName(); *physics) {
            physicsBytes_ = readAsset(root + physics);
            LoadPhysics(reinterpret_cast<const Csm::csmByte*>(physicsBytes_.data()), physicsBytes_.size());
        }
        for (int i = 0; i < _model->GetParameterCount(); ++i) initialParameters_.push_back(_model->GetParameterValue(i));
        for (int i = 0; i < _model->GetPartCount(); ++i) initialParts_.push_back(_model->GetPartOpacity(i));
        for (int i = 0; i < settings.GetEyeBlinkParameterCount(); ++i)
            eyes_.PushBack(settings.GetEyeBlinkParameterId(i));
        openness_.resize(eyes_.GetSize());
        _eyeBlink = Csm::CubismEyeBlink::Create(&settings);
        Csm::csmVector<Csm::CubismIdHandle> lips;
        for (int i = 0; i < settings.GetLipSyncParameterCount(); ++i)
            lips.PushBack(settings.GetLipSyncParameterId(i));
        for (int g = 0; g < settings.GetMotionGroupCount(); ++g) {
            const auto group = settings.GetMotionGroupName(g);
            const auto bytes = readAsset(root + settings.GetMotionFileName(group, 0));
            auto* motion = static_cast<Csm::CubismMotion*>(LoadMotion(
                reinterpret_cast<const Csm::csmByte*>(bytes.data()), bytes.size(), group,
                nullptr, nullptr, &settings, group, 0));
            if (!motion) throw std::runtime_error("Invalid Cubism motion: " + std::string(group));
            motion->SetLoop(false);
            if (stage) motion->SetLoopFadeIn(false);
            motion->SetEffectIds(eyes_, lips);
            motions_.emplace(group, motion);
        }
        // soundvoltex.dll 1806d93c0, constants 1809a1cb0..1809a1d38.
        _breath = Csm::CubismBreath::Create();
        Csm::csmVector<Csm::CubismBreath::BreathParameterData> breath;
        const auto id = Csm::CubismFramework::GetIdManager();
        breath.PushBack({id->GetId("ParamAngleX"), 0, 15, 6.5345f, .3f});
        breath.PushBack({id->GetId("ParamAngleY"), 0, 8, 3.5345f, .3f});
        breath.PushBack({id->GetId("ParamAngleZ"), 0, 10, 5.5345f, .3f});
        breath.PushBack({id->GetId("ParamBodyAngleX"), 0, 4, 15.5345f, .5f});
        breath.PushBack({id->GetId("ParamBreath"), .5f, .5f, 3.2345f, .5f});
        _breath->SetParameters(breath);
        _model->SaveParameters();
        for (int i = 0; i < settings.GetTextureCount(); ++i)
            texturePaths_.push_back(root + settings.GetTextureFileName(i));
        restoreGraphics(deferTextures);
    }

    void releaseGraphics() {
        DeleteRenderer();
        glDeleteTextures(textures_.size(), textures_.data());
        textures_.clear();
    }

    void restoreGraphics(bool deferTextures = false) {
        if (decoding_.valid()) decoding_.get();
        CreateRenderer(Width, Height, maskBuffers_);
        auto* renderer = GetRenderer<GlRenderer>();
        renderer->IsPremultipliedAlpha(true);
        textures_.resize(texturePaths_.size());
        glGenTextures(textures_.size(), textures_.data());
        nextTexture_ = 0;
        if (deferTextures) {
            if (!texturePaths_.empty()) decoding_ = std::async(std::launch::async, decodeModelTexture, texturePaths_.front());
        } else {
            while (nextTexture_ < texturePaths_.size()) uploadTexture(decodeModelTexture(texturePaths_[nextTexture_]));
        }
    }

    bool prepareGraphics() {
        if (!decoding_.valid()) return true;
        if (decoding_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        uploadTexture(decoding_.get());
        if (nextTexture_ == texturePaths_.size()) return true;
        decoding_ = std::async(std::launch::async, decodeModelTexture, texturePaths_[nextTexture_]);
        return false;
    }

    ~Model() override {
        _motionManager->StopAllMotions();
        for (const auto& [_, motion] : motions_) Csm::ACubismMotion::Delete(motion);
        glDeleteTextures(textures_.size(), textures_.data());
    }

    void start(std::string_view group, int priority, bool loop = false) {
        // 18062f180 cancels the old queue; the saved pose and physics survive.
        auto* motion = motions_.at(std::string(group));
        motion->SetLoop(loop);
        _motionManager->StopAllMotions();
        _motionManager->StartMotionPriority(motion, false, priority);
        if (fresh_) {
            advance(0);
            fresh_ = false;
        }
    }

    void reset() {
        _motionManager->StopAllMotions();
        for (size_t i = 0; i < initialParameters_.size(); ++i) _model->SetParameterValue(int(i), initialParameters_[i]);
        for (size_t i = 0; i < initialParts_.size(); ++i) _model->SetPartOpacity(int(i), initialParts_[i]);
        _model->SetModelOpacity(1);
        _model->SaveParameters();
        Csm::CubismEyeBlink::Delete(_eyeBlink);
        _eyeBlink = Csm::CubismEyeBlink::Create();
        _eyeBlink->SetParameterIds(eyes_);
        const auto parameters = _breath->GetParameters();
        Csm::CubismBreath::Delete(_breath);
        _breath = Csm::CubismBreath::Create();
        _breath->SetParameters(parameters);
        Csm::CubismPhysics::Delete(_physics);
        _physics = Csm::CubismPhysics::Create(reinterpret_cast<const Csm::csmByte*>(physicsBytes_.data()), physicsBytes_.size());
        fresh_ = true;
    }

    void advance(float seconds, bool updateDrawables = true) {
        _model->LoadParameters();
        const bool moved = !_motionManager->IsFinished() && _motionManager->UpdateMotion(_model, seconds);
        _model->SaveParameters();
        if (!moved) {
            for (unsigned i = 0; i < eyes_.GetSize(); ++i) openness_[i] = _model->GetParameterValue(eyes_[i]);
            _eyeBlink->UpdateParameters(_model, seconds);
            // 1806dbf00 -> 1806ee9c0 multiplies the saved expression by the
            // blink factor. SDK default assignment would open a held wink.
            for (unsigned i = 0; i < eyes_.GetSize(); ++i)
                _model->MultiplyParameterValue(eyes_[i], openness_[i]);
        }
        _breath->UpdateParameters(_model, seconds);
        if (_physics) _physics->Evaluate(_model, seconds);
        if (updateDrawables) _model->Update();
    }

    void advanceStage(int64_t ticks) {
        // Legacy physics files without Meta.Fps integrate once per Evaluate.
        // A fractional remainder can turn a normal displacement into a huge velocity.
        for (int64_t tick = 0; tick < ticks; ++tick)
            advance(1.f / 120, tick + 1 == ticks);
    }

    bool finished() const { return _motionManager->IsFinished(); }
    bool hasMotion(std::string_view group) const { return motions_.contains(std::string(group)); }

    double duration(std::string_view group) const {
        return motions_.at(std::string(group))->GetLoopDuration() * 1000.0;
    }

    void draw(CrewPlacement placement) {
        // NABLA initializes its model layout with 1/canvasWidth (1806e1582),
        // then applies 2*scale and a W/H projection (6dd180 / 6d7430).
        const float scale = 2 / _model->GetCanvasWidth();
        Csm::CubismMatrix44 matrix;
        float values[16] = {
            scale * placement.scaleX, 0, 0, 0,
            0, scale * placement.scaleY * Width / Height, 0, 0,
            0, 0, 1, 0,
            2 * placement.x / Width - 1, 1 - 2 * placement.y / Height, 0, 1
        };
        matrix.SetMatrix(values);
        auto* renderer = GetRenderer<GlRenderer>();
        renderer->SetMvpMatrix(&matrix);
        renderer->SetModelColor(1, 1, 1, _model->GetModelOpacity());
        GLint previousVao;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
        // The SDK's GLES2 backend uses client vertex arrays. GLES3 permits
        // those on the default VAO; keep the cabinet's buffered VAO untouched.
        glBindVertexArray(0);
        renderer->DrawModel();
        glBindVertexArray(previousVao);
    }

private:
    void uploadTexture(const ModelTexture& image) {
        const auto index = nextTexture_++;
        glBindTexture(GL_TEXTURE_2D, textures_[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.get());
        // The SDK's SetupTexture selects trilinear minification on draw.
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GetRenderer<GlRenderer>()->BindTexture(index, textures_[index]);
    }

    Csm::csmVector<Csm::CubismIdHandle> eyes_;
    std::unordered_map<std::string, Csm::CubismMotion*> motions_;
    std::vector<GLuint> textures_;
    std::vector<std::string> texturePaths_;
    int maskBuffers_ = 1;
    std::future<ModelTexture> decoding_;
    size_t nextTexture_ = 0;
    std::vector<float> initialParameters_, initialParts_;
    std::vector<float> openness_;
    std::string physicsBytes_;
    bool fresh_ = true;
};
}

struct CrewModel::Impl {
    std::shared_ptr<CubismRuntime> runtime = CubismRuntime::acquire();
    struct Actor {
        const CrewActor& data;
        std::unique_ptr<Model> model;
        bool visible = false;
    };
    std::vector<Actor> actors;
    int crew = -1;
};

CrewModel::CrewModel() : impl_(std::make_unique<Impl>()) {}
CrewModel::~CrewModel() = default;

void CrewModel::prepareShaders() {
    Csm::Rendering::CubismShader_OpenGLES2::GetInstance();
}
void CrewModel::select(int crew, bool reset, bool deferTextures) {
    auto& state = *impl_;
    if (state.crew != crew) {
        state.actors.clear();
        const auto& selected = crewInfo(crew);
        const auto& source = selected.actors.empty() && selected.roomAfp.empty() && selected.matchingAfp.empty()
            && selected.portrait.empty() ? crewInfo(1) : selected;
        for (int index : source.actors) {
            const auto& actor = crewActor(index);
            state.actors.push_back({actor, std::make_unique<Model>(actor.model, deferTextures)});
        }
        state.crew = crew;
    } else if (reset) {
        for (auto& actor : state.actors) {
            actor.model->reset();
            actor.visible = false;
        }
    }
}
bool CrewModel::prepareGraphics() {
    return std::ranges::all_of(impl_->actors, [](const auto& actor) { return actor.model->prepareGraphics(); });
}
void CrewModel::start(std::string_view group, int priority) {
    for (auto& actor : impl_->actors) {
        const auto alias = actor.data.motions.find(std::string(group));
        const auto motion = alias == actor.data.motions.end() ? group : std::string_view(alias->second);
        if (actor.model->hasMotion(motion)) {
            actor.model->start(motion, priority);
            actor.visible = true;
        }
    }
}
void CrewModel::update(float seconds) {
    for (auto& actor : impl_->actors)
        if (actor.visible) actor.model->advance(seconds);
}
bool CrewModel::finished() const {
    return std::all_of(impl_->actors.begin(), impl_->actors.end(), [](const auto& actor) {
        return !actor.visible || actor.model->finished();
    });
}
bool CrewModel::hasMotion(std::string_view group) const {
    return std::any_of(impl_->actors.begin(), impl_->actors.end(), [&](const auto& actor) {
        return actor.model->hasMotion(group);
    });
}
void CrewModel::draw(CrewScene scene) {
    for (const auto& actor : impl_->actors)
        if (actor.visible) actor.model->draw(actor.data.placements[static_cast<int>(scene)]);
}
void CrewModel::releaseGraphics() {
    for (auto& actor : impl_->actors) actor.model->releaseGraphics();
    Csm::Rendering::CubismRenderer::StaticRelease();
}
void CrewModel::restoreGraphics() {
    for (auto& actor : impl_->actors) actor.model->restoreGraphics();
}

struct StageModel::Impl {
    std::shared_ptr<CubismRuntime> runtime = CubismRuntime::acquire();
    Model model;
    CrewPlacement placement;
    double start = 0, end = 0;
    int64_t ticks = 0;
    std::string loop;
    bool active = false;

    Impl(const std::string& path, std::array<float, 4> position, bool deferTextures)
        : model(path, deferTextures, true), placement{position[0], position[1], position[2], position[3]} {}
};

StageModel::StageModel(const std::string& path, std::array<float, 4> placement, bool deferTextures)
    : impl_(std::make_unique<Impl>(path, placement, deferTextures)) {}
StageModel::~StageModel() = default;

void StageModel::setPlacement(std::array<float, 4> placement) {
    impl_->placement = {placement[0], placement[1], placement[2], placement[3]};
}

void StageModel::play(std::string_view motion, std::string_view loop, double time) {
    update(time);
    auto& state = *impl_;
    state.active = !motion.empty();
    if (!state.active) return;
    state.start = time;
    state.ticks = 0;
    state.end = time + state.model.duration(motion);
    state.loop = loop;
    state.model.start(motion, 3);
    state.model.advance(0);
}

void StageModel::update(double time) {
    auto& state = *impl_;
    if (!state.active || time <= state.start) return;
    const auto advanceTo = [&](double at) {
        const auto ticks = static_cast<int64_t>(std::floor((at - state.start) * .12));
        if (ticks > state.ticks) {
            state.model.advanceStage(ticks - state.ticks);
            state.ticks = ticks;
        }
    };
    if (time >= state.end) {
        advanceTo(state.end);
        if (state.loop.empty()) {
            state.active = false;
            return;
        }
        state.model.start(state.loop, 3, true);
        state.model.advance(0);
        state.end = std::numeric_limits<double>::infinity();
    }
    advanceTo(time);
}

void StageModel::draw(Render& render) {
    if (!impl_->active) return;
    render.flush();
    impl_->model.draw(impl_->placement);
}

void StageModel::releaseGraphics() { impl_->model.releaseGraphics(); }
bool StageModel::prepareGraphics() { return impl_->model.prepareGraphics(); }
void StageModel::restoreGraphics() { impl_->model.restoreGraphics(); }
}
