#include "s3v_audio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace sdvx {
namespace {
struct MemoryInput {
    std::string_view bytes;
    int64_t position = 0;

    static int read(void* user, uint8_t* output, int requested) {
        auto& input = *static_cast<MemoryInput*>(user);
        const auto count = std::min<int64_t>(requested, input.bytes.size() - input.position);
        if (!count) return AVERROR_EOF;
        std::memcpy(output, input.bytes.data() + input.position, count);
        input.position += count;
        return static_cast<int>(count);
    }

    static int64_t seek(void* user, int64_t offset, int mode) {
        auto& input = *static_cast<MemoryInput*>(user);
        const auto size = static_cast<int64_t>(input.bytes.size());
        switch (mode & ~AVSEEK_FORCE) {
            case AVSEEK_SIZE: return size;
            case SEEK_SET: break;
            case SEEK_CUR:
                if (offset < -input.position || offset > size - input.position) return AVERROR(EINVAL);
                offset += input.position;
                break;
            case SEEK_END:
                if (offset < -size || offset > 0) return AVERROR(EINVAL);
                offset += size;
                break;
            default: return AVERROR(EINVAL);
        }
        if (offset < 0 || offset > size) return AVERROR(EINVAL);
        return input.position = offset;
    }
};

struct AsfInput {
    AVFormatContext* format = avformat_alloc_context();
    AVIOContext* io = nullptr;

    ~AsfInput() {
        avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};
}

std::string decodeS3vWave(std::string_view source, int sampleRate) {
    if (source.starts_with("S3V0")) {
        if (source.size() < 12) return {};
        const auto* header = reinterpret_cast<const uint8_t*>(source.data());
        const auto read32 = [header](int index) {
            return uint32_t(header[index]) | uint32_t(header[index + 1]) << 8
                | uint32_t(header[index + 2]) << 16 | uint32_t(header[index + 3]) << 24;
        };
        const auto start = read32(4), length = read32(8);
        if (start < 12 || start > source.size() || length > source.size() - start) return {};
        source = source.substr(start, length);
    }

    MemoryInput input{source};
    AsfInput asf;
    if (!asf.format) return {};
    constexpr int bufferSize = 32768;
    auto* buffer = static_cast<uint8_t*>(av_malloc(bufferSize));
    if (!buffer) return {};
    asf.io = avio_alloc_context(buffer, bufferSize, 0, &input, MemoryInput::read, nullptr, MemoryInput::seek);
    if (!asf.io) {
        av_free(buffer);
        return {};
    }
    asf.format->pb = asf.io;
    asf.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&asf.format, nullptr, av_find_input_format("asf"), nullptr) < 0 ||
        avformat_find_stream_info(asf.format, nullptr) < 0) return {};

    const AVCodec* decoder = nullptr;
    const int stream = av_find_best_stream(asf.format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream < 0) return {};
    const auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> codec(avcodec_alloc_context3(decoder), freeCodec);
    if (!codec || avcodec_parameters_to_context(codec.get(), asf.format->streams[stream]->codecpar) < 0 ||
        avcodec_open2(codec.get(), decoder, nullptr) < 0) return {};

    const auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    const auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    if (!packet || !frame) return {};

    const int rate = sampleRate ? sampleRate : codec->sample_rate;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* converter = nullptr;
    const auto freeConverter = [](SwrContext* value) { swr_free(&value); };
    const int result = swr_alloc_set_opts2(&converter, &stereo, AV_SAMPLE_FMT_S16, rate,
        &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
    std::unique_ptr<SwrContext, decltype(freeConverter)> resampler(converter, freeConverter);
    if (result < 0 || swr_init(resampler.get()) < 0) return {};

    constexpr size_t headerSize = 44, frameBytes = 4;
    std::string wave(headerSize, '\0');
    auto append = [&](const uint8_t* const* data, int samples) {
        const int capacity = swr_get_out_samples(resampler.get(), samples);
        const auto offset = wave.size();
        if (capacity < 0 || size_t(capacity) * frameBytes > UINT32_MAX - offset) return false;
        wave.resize(offset + capacity * frameBytes);
        auto* destination = reinterpret_cast<uint8_t*>(wave.data() + offset);
        const int produced = swr_convert(resampler.get(), &destination, capacity, data, samples);
        if (produced < 0) return false;
        wave.resize(offset + produced * frameBytes);
        return true;
    };
    auto receive = [&] {
        int status;
        while ((status = avcodec_receive_frame(codec.get(), frame.get())) == 0) {
            if (!append(frame->extended_data, frame->nb_samples)) return false;
        }
        return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
    };
    int status;
    while ((status = av_read_frame(asf.format, packet.get())) >= 0) {
        if (packet->stream_index == stream &&
            (avcodec_send_packet(codec.get(), packet.get()) < 0 || !receive())) return {};
        av_packet_unref(packet.get());
    }
    if (status != AVERROR_EOF || avcodec_send_packet(codec.get(), nullptr) < 0 || !receive() ||
        !append(nullptr, 0) || wave.size() == headerSize) return {};

    std::memcpy(wave.data(), "RIFF", 4);
    std::memcpy(wave.data() + 8, "WAVEfmt ", 8);
    std::memcpy(wave.data() + 36, "data", 4);
    const auto write = [&](size_t offset, uint32_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) wave[offset + i] = static_cast<char>(value >> (8 * i));
    };
    write(4, wave.size() - 8, 4);
    write(16, 16, 4);
    write(20, 1, 2);
    write(22, 2, 2);
    write(24, rate, 4);
    write(28, rate * frameBytes, 4);
    write(32, frameBytes, 2);
    write(34, 16, 2);
    write(40, wave.size() - headerSize, 4);
    return wave;
}
}
