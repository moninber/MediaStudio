#include "LocalMediaFixtures.h"
#include "MediaDecoder.h"

#include <QDir>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// RAII 仅关闭本次创建的封装、文件和包，构造中途失败也能释放资源。
struct Output {
    AVFormatContext* format = nullptr;
    AVPacket* packet = av_packet_alloc();
    ~Output()
    {
        av_packet_free(&packet);
        if (format) { avio_closep(&format->pb); avformat_free_context(format); }
    }
};

// 24 秒 raw I420/PCM；每个视频间隔恰好对应 2940 个双声道采样，保留独立轨道时间基。
void writeMedia(const std::filesystem::path& path, bool has_audio, bool has_video)
{
    constexpr int width = 64, height = 48, fps = 15, sample_rate = 44100;
    constexpr int samples = sample_rate / fps;
    Output out;
    const auto filename = path.u8string();
    require(avformat_alloc_output_context2(&out.format, nullptr, has_video ? "avi" : "wav", filename.c_str()) >= 0 &&
        out.format && out.packet, "local fixture allocation failed");
    AVStream* video = has_video ? avformat_new_stream(out.format, nullptr) : nullptr;
    AVStream* audio = has_audio ? avformat_new_stream(out.format, nullptr) : nullptr;
    require((!has_video || video) && (!has_audio || audio), "local fixture streams failed");
    if (video) {
        video->time_base = {1, fps}; video->avg_frame_rate = {fps, 1};
        auto* codec = video->codecpar;
        codec->codec_type = AVMEDIA_TYPE_VIDEO; codec->codec_id = AV_CODEC_ID_RAWVIDEO;
        codec->codec_tag = MKTAG('I','4','2','0'); codec->format = AV_PIX_FMT_YUV420P;
        codec->width = width; codec->height = height;
    }
    if (audio) {
        audio->time_base = {1, sample_rate};
        auto* codec = audio->codecpar;
        codec->codec_type = AVMEDIA_TYPE_AUDIO; codec->codec_id = AV_CODEC_ID_PCM_S16LE;
        codec->format = AV_SAMPLE_FMT_S16; codec->sample_rate = sample_rate;
        codec->bits_per_coded_sample = 16; codec->block_align = 4; codec->bit_rate = sample_rate * 32;
        av_channel_layout_default(&codec->ch_layout, 2);
    }
    require(avio_open(&out.format->pb, filename.c_str(), AVIO_FLAG_WRITE) >= 0 &&
        avformat_write_header(out.format, nullptr) >= 0, "local fixture header failed");
    for (int frame = 0; frame < fps * 24; ++frame) {
        if (video) {
            require(av_new_packet(out.packet, width * height * 3 / 2) >= 0, "local fixture video allocation failed");
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                out.packet->data[y * width + x] = static_cast<std::uint8_t>(32 + (2*x+y+frame)%190);
            std::fill_n(out.packet->data+width*height, width*height/4, std::uint8_t{112});
            std::fill_n(out.packet->data+width*height*5/4, width*height/4, std::uint8_t{148});
            out.packet->stream_index = video->index; out.packet->pts = out.packet->dts = frame;
            out.packet->duration = 1; out.packet->flags = AV_PKT_FLAG_KEY;
            av_packet_rescale_ts(out.packet, {1,fps}, video->time_base);
            require(av_interleaved_write_frame(out.format, out.packet) >= 0, "local fixture video write failed");
        }
        if (audio) {
            require(av_new_packet(out.packet, samples * 4) >= 0, "local fixture audio allocation failed");
            for (int i = 0; i < samples; ++i) {
                const auto sample = static_cast<std::int64_t>(frame)*samples+i;
                for (int channel = 0; channel < 2; ++channel) {
                    const double phase = 6.283185307179586 * (channel ? 660 : 440) * sample / sample_rate;
                    const auto value = static_cast<std::int16_t>(8000 * std::sin(phase));
                    std::memcpy(out.packet->data+(i*2+channel)*2, &value, sizeof(value));
                }
            }
            out.packet->stream_index = audio->index; out.packet->pts = out.packet->dts = static_cast<std::int64_t>(frame)*samples;
            out.packet->duration = samples; out.packet->flags = AV_PKT_FLAG_KEY;
            av_packet_rescale_ts(out.packet, {1,sample_rate}, audio->time_base);
            require(av_interleaved_write_frame(out.format, out.packet) >= 0, "local fixture audio write failed");
        }
    }
    require(av_write_trailer(out.format) >= 0, "local fixture trailer failed");
}
}

LocalMediaFixtures::LocalMediaFixtures()
    : directory_(QDir(qEnvironmentVariable("MEDIASTUDIO_TEST_DATA_ROOT", QDir::tempPath()))
        .filePath("mediastudio-local-XXXXXX"))
{
    require(directory_.isValid(), "local fixture directory unavailable");
    writeMedia(audioVideo(), true, true);
    writeMedia(audioOnly(), true, false);
    writeMedia(videoOnly(), false, true);
}

std::filesystem::path LocalMediaFixtures::audioVideo() const { return directory_.filePath("audio-video.avi").toStdWString(); }
std::filesystem::path LocalMediaFixtures::audioOnly() const { return directory_.filePath("audio.wav").toStdWString(); }
std::filesystem::path LocalMediaFixtures::videoOnly() const { return directory_.filePath("video.avi").toStdWString(); }
