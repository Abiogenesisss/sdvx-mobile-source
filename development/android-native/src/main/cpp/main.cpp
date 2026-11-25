#include "app.h"
#include "application_clock.h"
#include "asset_directory.h"
#include "stage_archive.h"
#include "cabinet.h"
#include "stage.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <jni.h>
#include <android/api-level.h>
#include <android/performance_hint.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace sdvx {
namespace {
std::atomic<Uint32> rotaryEvent = 0;
std::atomic<Uint32> controllerEvent = 0;
ApplicationClock appClock;

class FramePerformance {
    void* library_ = nullptr;
    APerformanceHintSession* session_ = nullptr;
    int (*report_)(APerformanceHintSession*, int64_t) = nullptr;
    void (*close_)(APerformanceHintSession*) = nullptr;
public:
    FramePerformance() {
        // Native ADPF starts at API 33; keep the existing API 26 runtime usable.
        if (android_get_device_api_level() < 33) return;
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        const auto manager = reinterpret_cast<APerformanceHintManager* (*)()>(
            dlsym(library_, "APerformanceHint_getManager"));
        const auto create = reinterpret_cast<APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t)>(
            dlsym(library_, "APerformanceHint_createSession"));
        report_ = reinterpret_cast<decltype(report_)>(dlsym(library_, "APerformanceHint_reportActualWorkDuration"));
        close_ = reinterpret_cast<decltype(close_)>(dlsym(library_, "APerformanceHint_closeSession"));
        if (auto* service = manager()) {
            const int32_t thread = gettid();
            session_ = create(service, &thread, 1, 1000000000 / 120);
        }
    }
    FramePerformance(const FramePerformance&) = delete;
    FramePerformance& operator=(const FramePerformance&) = delete;
    ~FramePerformance() {
        if (session_) close_(session_);
        if (library_) dlclose(library_);
    }
    void report(int64_t duration) {
        if (session_) report_(session_, duration);
    }
};

double wallMs() { return SDL_GetTicksNS() / 1000000.0; }
double clockMs() { return appClock.now(wallMs()); }

bool SDLCALL queueLifecycleEvent(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        // SDL delivers these only to watchers. Queue them for the game loop
        // without invoking the watcher again through SDL_PushEvent.
        SDL_Event queued{};
        queued.type = event->type;
        queued.common.timestamp = SDL_GetTicksNS();
        SDL_PeepEvents(&queued, 1, SDL_ADDEVENT, 0, 0);
    }
    return true;
}

std::string savePath(std::string_view name) {
    static const std::string base = [] {
        char* path = SDL_GetPrefPath("local", "sdvx");
        std::string value = path ? path : "";
        SDL_free(path);
        return value;
    }();
    return base + std::string(name);
}

bool validAssetPath(std::string_view path) {
    return !path.empty() && path.front() != '/' && path.find_first_of("\\:") == std::string_view::npos &&
        path.find('\0') == std::string_view::npos && path != ".." && !path.starts_with("../") &&
        path.find("/../") == std::string_view::npos && !path.ends_with("/..");
}

const std::string& externalRoot() {
    static const std::string root = [] {
        auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        env->PushLocalFrame(3);
        auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
        auto method = env->GetMethodID(env->GetObjectClass(activity), "resourceDirectory", "()Ljava/lang/String;");
        auto value = static_cast<jstring>(env->CallObjectMethod(activity, method));
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string directory(chars);
        env->ReleaseStringUTFChars(value, chars);
        env->PopLocalFrame(nullptr);
        return directory;
    }();
    return root;
}

SDL_IOStream* openAsset(std::string_view path, bool externalOnly) {
    if (!validAssetPath(path)) return nullptr;
    if (!externalOnly)
        if (auto* io = SDL_IOFromFile(std::string(path).c_str(), "rb")) return io;
    if (!path.starts_with("music/") && !path.starts_with("crew/")) return nullptr;
    const auto& root = externalRoot();
    if (root.empty()) return nullptr;
    const auto file = root + "/" + std::string(path);
    // Android 16 checks the media database even for images inside our own
    // Android/media directory. Register extracted images before their first
    // open so .nomedia classification is ready without a failed read/retry.
    if (android_get_device_api_level() >= 36 &&
        (path.ends_with(".png") || path.ends_with(".webp") ||
         path.ends_with(".jpg") || path.ends_with(".jpeg"))) {
        auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        env->PushLocalFrame(3);
        auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
        auto method = env->GetMethodID(env->GetObjectClass(activity), "prepareExternalImage", "(Ljava/lang/String;)Z");
        const bool ready = env->CallBooleanMethod(activity, method, env->NewStringUTF(file.c_str()));
        env->PopLocalFrame(nullptr);
        if (!ready) {
            SDL_SetError("Could not register external image: %s", file.c_str());
            return nullptr;
        }
    }
    return SDL_IOFromFile(file.c_str(), "rb");
}

std::string readFile(SDL_IOStream* io) {
    if (!io) return {};
    size_t size = 0;
    void* data = SDL_LoadFile_IO(io, &size, true);
    if (!data) return {};
    std::string bytes(static_cast<char*>(data), size);
    SDL_free(data);
    return bytes;
}

bool assetExists(std::string_view path, bool externalOnly) {
    if (!validAssetPath(path)) return false;
    if (!externalOnly) {
        if (auto* io = SDL_IOFromFile(std::string(path).c_str(), "rb")) {
            SDL_CloseIO(io);
            return true;
        }
    }
    if (!path.starts_with("music/") && !path.starts_with("crew/")) return false;
    static AssetDirectory directory(externalRoot());
    return directory.contains(path);
}
}

std::unordered_set<std::string> externalMusicFolders() {
    std::unordered_set<std::string> names;
    const auto& root = externalRoot();
    if (root.empty()) return names;
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(std::filesystem::path(root) / "music", error); it != end; it.increment(error)) {
        if (it->is_directory(error)) names.insert(it->path().filename().string());
    }
    return names;
}

std::string readAsset(std::string_view path) {
    if (const auto* entry = stageArchiveEntry(path); entry && !entry->width) return readStageRange(*entry);
    return readFile(openAsset(path, false));
}
bool hasAsset(std::string_view path) { return stageArchiveEntry(path) || assetExists(path, false); }
std::string readExternalAsset(std::string_view path) { return readFile(openAsset(path, true)); }
bool hasExternalAsset(std::string_view path) { return assetExists(path, true); }

std::string readSave(std::string_view name) { return readFile(SDL_IOFromFile(savePath(name).c_str(), "rb")); }
double applicationTime() { return clockMs(); }

std::string localCardState() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    env->PushLocalFrame(4);
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    auto method = env->GetMethodID(env->GetObjectClass(activity), "localCardState", "()Ljava/lang/String;");
    auto value = static_cast<jstring>(env->CallObjectMethod(activity, method));
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string state(chars);
    env->ReleaseStringUTFChars(value, chars);
    env->PopLocalFrame(nullptr);
    return state;
}

std::future<bool> submitLocalPin(std::string pin) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    env->PushLocalFrame(1);
    auto activity = env->NewGlobalRef(static_cast<jobject>(SDL_GetAndroidActivity()));
    env->PopLocalFrame(nullptr);
    // PBKDF2 and enrolment's disk commit must not block the SDL render thread.
    // The task owns its Activity reference and result, never Cabinet state.
    // A discarded packaged_task future does not join the worker on Back.
    std::packaged_task<bool()> check([activity, pin = std::move(pin)] {
        auto* worker = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        worker->PushLocalFrame(2);
        auto method = worker->GetMethodID(worker->GetObjectClass(activity), "submitLocalPin", "(Ljava/lang/String;)Z");
        const bool accepted = worker->CallBooleanMethod(activity, method, worker->NewStringUTF(pin.c_str()));
        worker->PopLocalFrame(nullptr);
        worker->DeleteGlobalRef(activity);
        return accepted;
    });
    auto result = check.get_future();
    std::thread(std::move(check)).detach();
    return result;
}

void showSubscreen(bool landscape) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    env->PushLocalFrame(2);
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    auto method = env->GetMethodID(env->GetObjectClass(activity), "showSubscreen", "(Z)V");
    env->CallVoidMethod(activity, method, jboolean(landscape));
    env->PopLocalFrame(nullptr);
}

int keyboardHeight() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    env->PushLocalFrame(2);
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    auto method = env->GetMethodID(env->GetObjectClass(activity), "keyboardHeight", "()I");
    const int height = env->CallIntMethod(activity, method);
    env->PopLocalFrame(nullptr);
    return height;
}

std::string normalizeSearch(std::string_view value) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    env->PushLocalFrame(4);
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    auto method = env->GetMethodID(env->GetObjectClass(activity), "normalizeSearch", "([B)[B");
    auto bytes = env->NewByteArray(value.size());
    env->SetByteArrayRegion(bytes,0,value.size(),reinterpret_cast<const jbyte*>(value.data()));
    auto result = static_cast<jbyteArray>(env->CallObjectMethod(activity, method, bytes));
    std::string text(env->GetArrayLength(result), '\0');
    env->GetByteArrayRegion(result,0,text.size(),reinterpret_cast<jbyte*>(text.data()));
    env->PopLocalFrame(nullptr);
    return text;
}

int64_t localDay() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    env->PushLocalFrame(2);
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    auto method = env->GetMethodID(env->GetObjectClass(activity), "localDay", "()J");
    auto day = env->CallLongMethod(activity, method);
    env->PopLocalFrame(nullptr);
    return day;
}

void writeSave(std::string_view name, std::string_view contents) {
    auto path = savePath(name);
    auto temp = path + ".tmp";
    if (SDL_SaveFile(temp.c_str(), contents.data(), contents.size())) {
        if (std::rename(temp.c_str(), path.c_str()) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not replace %s", path.c_str());
        }
    }
}

namespace {
int run(SDL_Window* window) {
    Render render(window);
    Audio audio;
    Cabinet cabinet(render, audio, true);
    std::unique_ptr<Stage> stage, retiringStage;
    std::optional<Selection> selection;
    std::optional<Selection> loadingTrack;
    std::future<std::shared_ptr<const Stage::SongData>> songLoad;
    std::shared_ptr<const Stage::SongData> preparedSong;
    bool stageStarting = false;
    bool stageRestoring = false;
    PlayOptions options;
    using Activity = ApplicationClock::Activity;
    bool running = true;
    bool landscape = false;
    bool textInput = false;
    bool graphicsLost = false;
    bool firstFrame = true;
    std::optional<FramePerformance> framePerformance;
    unsigned keyboardRequest = 0;
    ControllerMode controllerMode = ControllerMode::None;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    env->PushLocalFrame(2);
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    auto controllerInputMode = env->GetMethodID(env->GetObjectClass(activity), "controllerInputMode", "()I");
    controllerMode = static_cast<ControllerMode>(env->CallIntMethod(activity, controllerInputMode));
    env->PopLocalFrame(nullptr);
    SDL_HideCursor();
    cabinet.setControllerMode(controllerMode);
    SDL_SetWindowRelativeMouseMode(window, controllerMode != ControllerMode::Joystick);
    auto dispatch = [&](GameInput input) {
        if (appClock.activity() != Activity::Active) return;
        if (stage) {
            if (!cabinet.stageTransition() || input.type == GameInput::Type::FocusLost) stage->input(input);
        } else cabinet.input(input);
    };
    auto rotary = [&](double time, float x, float y) {
        dispatch({GameInput::Type::Rotary, time, 0, -1, x, y,
                  cabinet.rotaryDelta(0, x, y), cabinet.rotaryDelta(1, x, y)});
    };
    while (running) {
        const auto frameStarted = std::chrono::steady_clock::now();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const double time = appClock.eventTime(event.common.timestamp / 1000000.0);
            if (event.type == rotaryEvent.load(std::memory_order_relaxed)) {
                rotary(time, std::bit_cast<float>(event.user.code), std::bit_cast<float>(event.user.windowID));
                continue;
            }
            if (event.type == controllerEvent.load(std::memory_order_relaxed)) {
                const int key = event.user.code;
                if (key == 0) {
                    const auto mode = static_cast<ControllerMode>(event.user.windowID);
                    if (stage) stage->setTouchControls(mode == ControllerMode::None, clockMs());
                    controllerMode = mode;
                    cabinet.setControllerMode(controllerMode);
                    SDL_SetWindowRelativeMouseMode(window, controllerMode != ControllerMode::Joystick);
                } else if (controllerMode == ControllerMode::Joystick) {
                    const auto type = event.user.windowID ? GameInput::Type::KeyDown : GameInput::Type::KeyUp;
                    GameInput input{type, time, 0, cabinet.mappedControllerButton(key)};
                    input.controllerKey = key;
                    dispatch(input);
                }
                continue;
            }
            switch (event.type) {
                case SDL_EVENT_TEXT_INPUT:
                case SDL_EVENT_TEXT_EDITING: {
                    GameInput input{event.type == SDL_EVENT_TEXT_INPUT ? GameInput::Type::Text : GameInput::Type::Composition, time};
                    input.text = event.type == SDL_EVENT_TEXT_INPUT ? event.text.text : event.edit.text;
                    dispatch(std::move(input));
                    break;
                }
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WILL_ENTER_BACKGROUND: {
                    const auto at = event.common.timestamp / 1000000.0;
                    if (appClock.activity() == Activity::Active) {
                        dispatch({GameInput::Type::FocusLost, appClock.now(at)});
                        if (audio.suspend(true)) SDL_Log("SDVX paused: simulation and audio stopped");
                    }
                    appClock.enterBackground(at);
                    break;
                }
                case SDL_EVENT_DID_ENTER_FOREGROUND:
                    // Some drivers restore the EGL context handle but lose its
                    // textures without SDL emitting RENDER_DEVICE_RESET.
                    if (appClock.enterForeground()) graphicsLost = true;
                    break;
                case SDL_EVENT_RENDER_DEVICE_RESET:
                    graphicsLost = true;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    dispatch({GameInput::Type::FocusLost, time});
                    break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    SDL_HideCursor();
                    SDL_SetWindowRelativeMouseMode(window, controllerMode != ControllerMode::Joystick);
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP: {
                    bool down = event.type == SDL_EVENT_KEY_DOWN;
                    int key = event.key.scancode;
                    auto type = down ? GameInput::Type::KeyDown : GameInput::Type::KeyUp;
                    if (key == SDL_SCANCODE_AC_BACK && down) type = GameInput::Type::Back;
                    const int mapped = cabinet.mappedButton(key);
                    const int button = controllerMode == ControllerMode::Joystick && mapped != Subscreen ? -1 : mapped;
                    dispatch({type, time, key, button, 0, 0, 0, 0, event.key.repeat});
                    break;
                }
                case SDL_EVENT_MOUSE_MOTION: {
                    if (controllerMode == ControllerMode::Joystick ||
                        event.motion.which == SDL_TOUCH_MOUSEID || event.motion.which == SDL_PEN_MOUSEID) break;
                    rotary(time, event.motion.xrel, event.motion.yrel);
                    break;
                }
                case SDL_EVENT_MOUSE_WHEEL: {
                    if (controllerMode == ControllerMode::Joystick) break;
                    float x = event.wheel.x, y = event.wheel.y;
                    dispatch({GameInput::Type::Scroll, time, 0, -1, x, y,
                              cabinet.rotaryDelta(0, x * 20, y * 20), cabinet.rotaryDelta(1, x * 20, y * 20)});
                    break;
                }
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION:
                case SDL_EVENT_FINGER_CANCELED:
                case SDL_EVENT_FINGER_UP: {
                    if (event.tfinger.touchID == SDL_MOUSE_TOUCHID) break;
                    Vec2 point = render.logical(event.tfinger.x, event.tfinger.y);
                    auto type = event.type == SDL_EVENT_FINGER_DOWN ? GameInput::Type::TouchDown
                        : event.type == SDL_EVENT_FINGER_UP ? GameInput::Type::TouchUp
                        : event.type == SDL_EVENT_FINGER_CANCELED ? GameInput::Type::TouchCancel : GameInput::Type::TouchMove;
                    GameInput input{type, time, 0, -1, point.x, point.y};
                    input.finger = event.tfinger.fingerID;
                    dispatch(input);
                    break;
                }
                default:
                    break;
            }
        }
        if (!running) break;
        if (appClock.activity() == Activity::Background) {
            framePerformance.reset();
            // With no GL surface, wait for lifecycle/input events, not a polling
            // render loop. Passing nullptr leaves the next event for PollEvent.
            SDL_WaitEvent(nullptr);
            continue;
        }
        if (graphicsLost) {
            framePerformance.reset();
            if (eglGetCurrentContext() == EGL_NO_CONTEXT || eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE
                || !render.displayReady(landscape)) {
                SDL_WaitEventTimeout(nullptr, 16);
                continue;
            }
            if (stage) stage->releaseGraphics();
            if (retiringStage) retiringStage->releaseGraphics();
            cabinet.releaseGraphics();
            render.resetGraphics();
            cabinet.restoreGraphics();
            if (stage) {
                stage->restoreGraphics();
                stageRestoring = true;
            }
            if (retiringStage) retiringStage->restoreGraphics();
            graphicsLost = false;
            SDL_Log("SDVX graphics restored");
        }
        if (stageRestoring && appClock.activity() == Activity::Resuming) {
            if (!stage->prepareGraphics()) {
                render.prepareStageImages();
                SDL_Delay(1);
                continue;
            }
            stageRestoring = false;
        }
        if (appClock.activity() == Activity::Resuming) {
            if (!audio.suspend(false)) {
                SDL_WaitEventTimeout(nullptr, 16);
                continue;
            }
            appClock.resume(wallMs());
            SDL_Log("SDVX resumed: active clock preserved");
        }
        double now = clockMs();
        const bool subscreen = !stage && cabinet.landscape();
        if (landscape != subscreen) {
            landscape = subscreen;
            showSubscreen(landscape);
            render.setLandscape(landscape);
        }
        cabinet.setDisplayReady(render.displayReady(landscape), now);
        // Android's IME back key stops SDL text input without sending a game BACK.
        if (textInput && cabinet.wantsTextInput() && !SDL_TextInputActive(window))
            cabinet.input(GameInput{GameInput::Type::Back});
        const bool wantsText = !stage && cabinet.wantsTextInput();
        if (wantsText) {
            int width, height;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            cabinet.setKeyboardTop(render.logical(0,1.f-float(keyboardHeight())/std::max(1,height)).y);
        }
        if (wantsText != textInput || (wantsText && keyboardRequest != cabinet.keyboardRequest())) {
            textInput = wantsText;
            keyboardRequest = cabinet.keyboardRequest();
            if (textInput) {
                int width, height;
                SDL_GetWindowSize(window, &width, &height);
                const SDL_Rect inputArea{width/4, height*5/9, width*13/24, height/18};
                SDL_SetTextInputArea(window, &inputArea, 0);
                SDL_StartTextInput(window);
            }
            else SDL_StopTextInput(window);
        }
        if (stage) {
            if (cabinet.stageTransition()) cabinet.update(now);
            if (!stageStarting && !stageRestoring && (!cabinet.stageTransition() || cabinet.matchingDeparture())) stage->update(now);
            if (auto result = stage->takeResult()) {
                options = stage->options();
                cabinet.applyStageOptions(options);
                if (result->retry && now < options.practiceUntil) {
                    cabinet.stageRetried();
                    stage.reset();
                    stage = std::make_unique<Stage>(render, audio, *selection, options, cabinet.playerCard(),
                                                    cabinet.bestScore(*selection), preparedSong, true);
                    stage->setTouchControls(controllerMode == ControllerMode::None, now);
                    stageRestoring = true;
                } else {
                    retiringStage = std::move(stage);
                    selection.reset();
                    preparedSong.reset();
                    result->cancelled |= result->retry;
                    cabinet.stageFinished(*result, now);
                }
            }
        } else {
            // Keep the outgoing stage visible through the glass, then release its audio before entering RESULT.
            if (retiringStage && !cabinet.stageClosing(now)) {
                retiringStage.reset();
                if (!loadingTrack) render.releaseStageData();
            }
            cabinet.update(now);
            auto next = cabinet.takeSelection();
            if (next) {
                selection = std::move(next);
                options = cabinet.options();
            }
            const auto* wanted = selection ? &*selection : cabinet.stagePreparation();
            const bool ready = songLoad.valid()
                && songLoad.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            const bool graphicsReady = render.afpDataReady();
            const bool sameTrack = loadingTrack && wanted && loadingTrack->id == wanted->id && loadingTrack->folder == wanted->folder
                && loadingTrack->chart == wanted->chart && loadingTrack->music == wanted->music;
            if ((ready || preparedSong) && !sameTrack && graphicsReady) {
                // A player can go back from MATCHING. Let its worker finish
                // without joining on the render thread or publishing stale data.
                songLoad = {};
                preparedSong.reset();
                loadingTrack.reset();
                if (!retiringStage) {
                    render.releaseStageImages();
                    render.releaseStageData();
                }
            }
            if (wanted && !loadingTrack) {
                loadingTrack = *wanted;
                songLoad = std::async(std::launch::async, Stage::prepareSong, *wanted);
                Stage::queueGraphics(render, *wanted);
            } else if ((ready || preparedSong) && sameTrack && graphicsReady && !retiringStage) {
                // Common stage art can upload during NEXT AREA and MATCHING.
                // The chart-specific queue is added when the Stage is constructed.
                if (ready) {
                    preparedSong = songLoad.get();
                    Stage::queuePreparedImages(render, *preparedSong);
                }
                if (!next && selection) {
                    loadingTrack.reset();
                    SDL_Log("Stage open %s %s", selection->folder.c_str(), selection->difficulty.c_str());
                    stage = std::make_unique<Stage>(render, audio, *selection, options, cabinet.playerCard(),
                                                    cabinet.bestScore(*selection), preparedSong, true);
                    stage->setTouchControls(controllerMode == ControllerMode::None, now);
                    stageStarting = true;
                    now = clockMs();
                }
            }
        }
        // A page may request rotation during this update. Apply it before its
        // first draw and block input until Android has resized the surface.
        if (landscape != (!stage && cabinet.landscape())) {
            landscape = !stage && cabinet.landscape();
            showSubscreen(landscape);
            render.setLandscape(landscape);
            cabinet.setDisplayReady(render.displayReady(landscape), now);
        }
        if (!render.displayReady(landscape)) {
            SDL_Delay(8);
            continue;
        }
        const bool drawStage = stage && ((!stageStarting && !stageRestoring) || stage->prepareGraphics())
            && cabinet.stageDepartureComplete(now);
        if (drawStage) stageRestoring = false;
        render.begin();
        if (drawStage) {
            if (!framePerformance) framePerformance.emplace();
            stage->draw(now);
            cabinet.drawStageShutter(now);
        } else {
            framePerformance.reset();
            if (retiringStage) retiringStage->draw(now);
            cabinet.draw(now);
        }
        render.end();
        if (framePerformance) framePerformance->report(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - frameStarted).count());
        if (firstFrame) {
            firstFrame = false;
            SDL_Log("SDVX first frame: %.0f ms since SDL startup", wallMs());
        }
        cabinet.preload();
        if (stageStarting && drawStage) {
            stageStarting = false;
            cabinet.stageLoaded(clockMs());
        }
    }
    return 0;
}
}
}

extern "C" JNIEXPORT void JNICALL Java_local_sdvx_nativeport_SdvxActivityKt_nativeRotary(
    JNIEnv*, jclass, jfloat x, jfloat y, jlong ageMs) {
    Uint32 type = sdvx::rotaryEvent.load(std::memory_order_relaxed);
    if (!type) return;
    Uint64 now = SDL_GetTicksNS();
    Uint64 age = Uint64(std::max<jlong>(0, ageMs)) * 1000000;
    SDL_Event event{};
    event.type = type;
    event.user.timestamp = now - std::min(now, age);
    // User events have no window target. Store the two float samples without allocating.
    event.user.code = std::bit_cast<Sint32>(x);
    event.user.windowID = std::bit_cast<Uint32>(y);
    SDL_PushEvent(&event);
}

extern "C" JNIEXPORT void JNICALL Java_local_sdvx_nativeport_SdvxActivityKt_nativeControllerInput(
    JNIEnv*, jclass, jint key, jint value, jlong ageMs) {
    const Uint32 type = sdvx::controllerEvent.load(std::memory_order_relaxed);
    if (!type) return;
    const Uint64 now = SDL_GetTicksNS();
    const Uint64 age = Uint64(std::max<jlong>(0, ageMs)) * 1000000;
    SDL_Event event{};
    event.type = type;
    event.user.timestamp = now - std::min(now, age);
    event.user.code = key;
    event.user.windowID = value;
    SDL_PushEvent(&event);
}

int main(int, char**) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL init: %s", SDL_GetError());
        return 1;
    }
    if (!SDL_AddEventWatch(sdvx::queueLifecycleEvent, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL lifecycle watch: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    sdvx::rotaryEvent.store(SDL_RegisterEvents(1), std::memory_order_relaxed);
    sdvx::controllerEvent.store(SDL_RegisterEvents(1), std::memory_order_relaxed);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef SDVX_EMULATOR_GLES31
    constexpr int minor = 1;
#else
    constexpr int minor = 2;
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    auto window = SDL_CreateWindow("SDVX ∇", 1080, 1920, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    auto context = window ? SDL_GL_CreateContext(window) : nullptr;
    int status = 1;
    try {
        if (!context) throw std::runtime_error(SDL_GetError());
        GLint majorVersion = 0, minorVersion = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
        glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
        if (majorVersion < 3 || (majorVersion == 3 && minorVersion < minor)) {
            throw std::runtime_error("The Android GLES driver did not create the required context");
        }
        SDL_Log("GLES %s, GLSL %s", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
        SDL_DisableScreenSaver();
        SDL_GL_SetSwapInterval(1);
        status = sdvx::run(window);
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SDVX ∇", e.what(), window);
    }
    SDL_RemoveEventWatch(sdvx::queueLifecycleEvent, nullptr);
    // SDL may have replaced the original context during an Android resume.
    if (const auto current = SDL_GL_GetCurrentContext()) SDL_GL_DestroyContext(current);
    if (window) SDL_DestroyWindow(window);
    sdvx::rotaryEvent.store(0, std::memory_order_relaxed);
    sdvx::controllerEvent.store(0, std::memory_order_relaxed);
    SDL_Quit();
    return status;
}
