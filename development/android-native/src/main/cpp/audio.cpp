#include "app.h"
#include "s3v_audio.h"

#include <SDL3/SDL.h>
#include <bass.h>
#include <bass_fx.h>

#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <thread>

namespace sdvx {
namespace {
std::string readAudio(std::string_view path, bool external = false) {
    auto bytes = external ? readExternalAsset(path) : readAsset(path);
    if (!bytes.empty() && path.ends_with(".s3v")) return decodeS3vWave(bytes, 0);
    return bytes;
}
}

struct Audio::Impl {
    HSTREAM music = 0, preview = 0, effectLoop = 0;
    struct PreviewLoad {
        std::string path;
        bool external;
        std::future<std::string> bytes;
    };
    std::optional<PreviewLoad> previewLoad;
    std::shared_ptr<const std::string> musicData;
    std::vector<HCHANNEL> narration;
    HDSP dsp = 0;
    HSTREAM outputMix = 0;
    HDSP outputLimiter = 0;
    DWORD outputChannels = 0;
    float limiterGain = 1, limiterRelease = 0;
    Dsp process = nullptr;
    void* context = nullptr;
    std::unordered_map<std::string, HSAMPLE> samples;
    double duration = 0;
    bool started = false;
    bool narrationEnabled = true;

    HSAMPLE sample(std::string_view path, int simultaneous) {
        auto [entry, inserted] = samples.try_emplace(std::string(path), 0);
        if (inserted) {
            const auto bytes = readAsset(path);
            if (!bytes.empty())
                entry->second = BASS_SampleLoad(BASS_FILE_MEM, bytes.data(), 0, bytes.size(), simultaneous, BASS_SAMPLE_OVER_POS);
        }
        return entry->second;
    }

    HCHANNEL playSample(std::string_view path, float gain, int simultaneous = 8, double offsetMs = 0) {
        const auto handle = sample(path, simultaneous);
        if (!handle) return 0;
        const HCHANNEL channel = BASS_SampleGetChannel(handle, 0);
        if (!channel) return 0;
        BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, gain);
        if (offsetMs <= 0) return BASS_ChannelPlay(channel, true) ? channel : 0;
        const auto offset = BASS_ChannelSeconds2Bytes(channel, offsetMs / 1000);
        return offset < BASS_ChannelGetLength(channel, BASS_POS_BYTE)
            && BASS_ChannelSetPosition(channel, offset, BASS_POS_BYTE)
            && BASS_ChannelPlay(channel, false) ? channel : 0;
    }

    static void CALLBACK applyDsp(HDSP, DWORD channel, void* buffer, DWORD length, void* user) {
        auto& self = *static_cast<Impl*>(user);
        constexpr int BytesPerFrame = sizeof(float) * 2;
        QWORD first = BASS_ChannelGetPosition(channel, BASS_POS_BYTE | BASS_POS_DSP);
        if (first == static_cast<QWORD>(-1)) return;
        self.process(self.context, static_cast<float*>(buffer), length / BytesPerFrame, first / BytesPerFrame);
    }

    static void CALLBACK limitOutput(HDSP, DWORD, void* buffer, DWORD length, void* user) {
        auto& self = *static_cast<Impl*>(user);
        auto* samples = static_cast<float*>(buffer);
        const auto frames = length / (sizeof(float) * self.outputChannels);
        constexpr float ceiling = .89125094f; // -1 dBFS
        float gain = self.limiterGain;
        for (size_t i = 0; i < frames; ++i, samples += self.outputChannels) {
            float peak = 0;
            for (DWORD ch = 0; ch < self.outputChannels; ++ch) peak = std::max(peak, std::abs(samples[ch]));
            // Link the channels to keep the stereo image during peaks.
            // Recover over multiple blocks instead of following the waveform.
            gain += (1 - gain) * self.limiterRelease;
            if (peak * gain > ceiling) gain = ceiling / peak;
            if (gain < 1)
                for (DWORD ch = 0; ch < self.outputChannels; ++ch) samples[ch] *= gain;
        }
        self.limiterGain = gain;
    }

    void prepareOutputLimiter() {
        const auto mix = BASS_StreamCreate(0, 0, 0, STREAMPROC_DEVICE, nullptr);
        if (mix && mix == outputMix && outputLimiter) return;
        BASS_CHANNELINFO info{};
        if (mix && BASS_ChannelGetInfo(mix, &info)) {
            outputMix = mix;
            outputChannels = info.chans;
            limiterGain = 1;
            limiterRelease = 1 - std::exp(-1.f / (.06f * info.freq));
            outputLimiter = BASS_ChannelSetDSP(outputMix, limitOutput, this, 0);
            if (outputLimiter) return;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Stage output limiter unavailable: BASS %d", BASS_ErrorGetCode());
    }
};

Audio::Audio() : impl_(std::make_unique<Impl>()) {
    BASS_SetConfig(BASS_CONFIG_DEV_PERIOD, 5);
    BASS_SetConfig(BASS_CONFIG_DEV_BUFFER, 20);
    if (!BASS_Init(-1, SampleRate, 0, nullptr, nullptr)) {
        throw std::runtime_error("BASS_Init failed: " + std::to_string(BASS_ErrorGetCode()));
    }
    SDL_Log("BASS version=%08x BASS_FX=%08x", BASS_GetVersion(), BASS_FX_GetVersion());
}

Audio::~Audio() {
    stopMusic();
    stopPreview();
    stopEffectLoop();
    BASS_Free();
}

void Audio::setGain(float gain) {
    const DWORD volume = static_cast<DWORD>(gain * 10000);
    BASS_SetConfig(BASS_CONFIG_GVOL_STREAM, volume);
    BASS_SetConfig(BASS_CONFIG_GVOL_SAMPLE, volume);
}

bool Audio::prepareMusic(std::string_view path, bool loop, float gain) {
    return prepareMusicData(path, {}, loop, gain);
}

bool Audio::prepareMusicData(std::string_view path, std::shared_ptr<const std::string> bytes, bool loop, float gain) {
    stopMusic();
    if (!bytes) bytes = std::make_shared<const std::string>(readAudio(path));
    if (bytes->empty()) return false;
    DWORD flags = BASS_SAMPLE_FLOAT | (loop ? BASS_SAMPLE_LOOP : 0);
    impl_->music = BASS_StreamCreateFile(BASS_FILE_MEM, bytes->data(), 0, bytes->size(), flags);
    if (!impl_->music) {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Could not load %.*s: BASS %d",
                     int(path.size()), path.data(), BASS_ErrorGetCode());
        return false;
    }
    impl_->musicData = std::move(bytes);
    BASS_ChannelSetAttribute(impl_->music, BASS_ATTRIB_VOL, gain);
    impl_->duration = 1000 * BASS_ChannelBytes2Seconds(impl_->music, BASS_ChannelGetLength(impl_->music, BASS_POS_BYTE));
    return true;
}

void Audio::startMusic() {
    if (impl_->music) impl_->started = BASS_ChannelPlay(impl_->music, false);
}

bool Audio::playMusic(std::string_view path, bool loop, float gain) {
    if (!prepareMusic(path, loop, gain)) return false;
    startMusic();
    return impl_->started;
}

void Audio::pauseMusic() {
    if (impl_->music) BASS_ChannelPause(impl_->music);
}

void Audio::stopMusic() {
    if (impl_->music) {
        BASS_StreamFree(impl_->music);
        impl_->music = 0;
        impl_->dsp = 0;
        impl_->musicData.reset();
    }
    if (impl_->outputLimiter) {
        BASS_ChannelRemoveDSP(impl_->outputMix, impl_->outputLimiter);
        impl_->outputLimiter = 0;
    }
    impl_->process = nullptr;
    impl_->context = nullptr;
    impl_->started = false;
    impl_->duration = 0;
}

double Audio::positionMs() const {
    if (!impl_->music) return 0;
    QWORD pos = BASS_ChannelGetPosition(impl_->music, BASS_POS_BYTE);
    return pos == static_cast<QWORD>(-1) ? 0 : 1000 * BASS_ChannelBytes2Seconds(impl_->music, pos);
}

double Audio::durationMs() const { return impl_->duration; }

bool Audio::musicEnded() const {
    return impl_->started && BASS_ChannelIsActive(impl_->music) == BASS_ACTIVE_STOPPED;
}

void Audio::setDsp(Dsp process, void* context) {
    if (!impl_->music) return;
    BASS_ChannelLock(impl_->music, true);
    if (impl_->dsp) {
        BASS_ChannelRemoveDSP(impl_->music, impl_->dsp);
        impl_->dsp = 0;
    }
    impl_->process = process;
    impl_->context = context;
    if (process) {
        BASS_CHANNELINFO info{};
        if (BASS_ChannelGetInfo(impl_->music, &info) && info.chans == 2 && info.freq == SampleRate) {
            BASS_ChannelSetAttribute(impl_->music, BASS_ATTRIB_BUFFER, 0);
            impl_->dsp = BASS_ChannelSetDSP(impl_->music, Impl::applyDsp, impl_.get(), 0);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Stage DSP expects 48 kHz stereo PCM");
        }
    }
    BASS_ChannelLock(impl_->music, false);
    if (process) impl_->prepareOutputLimiter();
}

void Audio::sfx(std::string_view path, float gain, int simultaneous, double offsetMs) {
    impl_->playSample(path, gain, simultaneous, offsetMs);
}

void Audio::prepareSample(std::string_view path, int simultaneous) {
    impl_->sample(path, simultaneous);
}

void Audio::startEffectLoop(std::string_view path, float gain, uint64_t loopStartBytes) {
    stopEffectLoop();
    const auto bytes = readAsset(path);
    if (bytes.empty()) return;
    impl_->effectLoop = BASS_StreamCreateFile(BASS_FILE_MEMCOPY, bytes.data(), 0, bytes.size(), BASS_SAMPLE_LOOP);
    if (!impl_->effectLoop) return;
    if (loopStartBytes) BASS_ChannelSetPosition(impl_->effectLoop, loopStartBytes, BASS_POS_LOOP);
    BASS_ChannelSetAttribute(impl_->effectLoop, BASS_ATTRIB_VOL, gain);
    BASS_ChannelPlay(impl_->effectLoop, false);
}

void Audio::stopEffectLoop() {
    if (impl_->effectLoop) {
        BASS_StreamFree(impl_->effectLoop);
        impl_->effectLoop = 0;
    }
}

void Audio::announce(std::string_view path, bool interrupt) {
    announce(path, interrupt, 1);
}

void Audio::announce(std::string_view path, bool interrupt, float gain) {
    if (!impl_->narrationEnabled) return;
    if (interrupt) stopNarration();
    std::erase_if(impl_->narration, [](HCHANNEL channel) {
        return BASS_ChannelIsActive(channel) == BASS_ACTIVE_STOPPED;
    });
    const auto bytes = readAsset(path);
    const auto channel = bytes.empty() ? 0 : BASS_StreamCreateFile(BASS_FILE_MEMCOPY, bytes.data(), 0,
                                                                   bytes.size(), BASS_STREAM_AUTOFREE);
    if (channel) {
        BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, gain);
        if (BASS_ChannelPlay(channel, false)) {
            impl_->narration.push_back(channel);
            SDL_Log("SDVX Voice %.*s", int(path.size()), path.data());
            return;
        }
        BASS_StreamFree(channel);
    }
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Could not play voice %.*s", int(path.size()), path.data());
}

void Audio::setNarrationEnabled(bool enabled) {
    impl_->narrationEnabled = enabled;
    if (!enabled) stopNarration();
}

void Audio::stopNarration() {
    for (const auto channel : impl_->narration) BASS_ChannelStop(channel);
    impl_->narration.clear();
}

bool Audio::narrating() const {
    return std::any_of(impl_->narration.begin(), impl_->narration.end(), [](HCHANNEL channel) {
        return BASS_ChannelIsActive(channel) != BASS_ACTIVE_STOPPED;
    });
}

bool Audio::previewReady(std::string_view path, bool external) {
    if (!path.ends_with(".s3v")) return true;
    auto& load = impl_->previewLoad;
    if (!load || load->path != path || load->external != external) {
        std::packaged_task<std::string()> decode([path = std::string(path), external] { return readAudio(path, external); });
        load = Impl::PreviewLoad{std::string(path), external, decode.get_future()};
        std::thread(std::move(decode)).detach();
    }
    return load->bytes.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

bool Audio::preview(std::string_view path, float gain, bool loop, bool external) {
    auto load = std::move(impl_->previewLoad);
    stopPreview();
    const auto bytes = load && load->path == path && load->external == external ? load->bytes.get() : readAudio(path, external);
    if (!bytes.empty())
        impl_->preview = BASS_StreamCreateFile(BASS_FILE_MEMCOPY, bytes.data(), 0, bytes.size(), loop ? BASS_SAMPLE_LOOP : 0);
    if (impl_->preview) {
        BASS_ChannelSetAttribute(impl_->preview, BASS_ATTRIB_VOL, gain);
        if (BASS_ChannelPlay(impl_->preview, true)) {
            if (impl_->music) BASS_ChannelSetAttribute(impl_->music, BASS_ATTRIB_VOL, 0);
            return true;
        }
    }
    stopPreview(true);
    return false;
}

double Audio::previewPositionMs() const {
    const auto channel = impl_->preview;
    return 1000 * BASS_ChannelBytes2Seconds(channel, BASS_ChannelGetPosition(channel, BASS_POS_BYTE));
}

void Audio::stopPreview(bool restoreMusic) {
    impl_->previewLoad.reset();
    if (impl_->preview) {
        BASS_StreamFree(impl_->preview);
        impl_->preview = 0;
    }
    if (restoreMusic && impl_->music) {
        float gain = 1;
        BASS_ChannelGetAttribute(impl_->music, BASS_ATTRIB_VOL, &gain);
        // The original mixer restores 0.33 gain per second (1807338a0).
        if (gain < 1) BASS_ChannelSlideAttribute(impl_->music, BASS_ATTRIB_VOL, 1, DWORD((1 - gain) * 1000 / .33f));
    }
}

void Audio::updatePreview() {
    if (impl_->preview && BASS_ChannelIsActive(impl_->preview) == BASS_ACTIVE_STOPPED) stopPreview(true);
}

bool Audio::suspend(bool paused) {
    if (paused) return !BASS_IsStarted() || BASS_Pause();
    return BASS_Start();
}
}
