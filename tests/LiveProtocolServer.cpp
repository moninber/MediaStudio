#include "LiveProtocolServer.h"
#include "MediaDecoder.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/base64.h>
}

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Winsock {
    Winsock() { WSADATA data{}; require(WSAStartup(MAKEWORD(2,2), &data) == 0, "fixture WSAStartup failed"); }
    ~Winsock() { WSACleanup(); }
};
struct Encoder {
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    ~Encoder() { av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec); }
};
struct Output {
    AVFormatContext* format = nullptr;
    AVPacket* packet = av_packet_alloc();
    ~Output() {
        av_packet_free(&packet);
        if (format) { avio_closep(&format->pb); avformat_free_context(format); }
    }
};

// 查询系统端口表，不用探测连接消耗 FFmpeg 单客户端 listener，也不与其并发抢 bind。
bool listening(unsigned short port, bool udp)
{
    DWORD size = 0;
    if (udp) GetUdpTable(nullptr, &size, false); else GetTcpTable(nullptr, &size, false);
    std::vector<unsigned char> bytes(size);
    if (udp) {
        auto* table = reinterpret_cast<MIB_UDPTABLE*>(bytes.data());
        if (GetUdpTable(table, &size, false) != NO_ERROR) return false;
        for (DWORD i = 0; i < table->dwNumEntries; ++i) if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == port) return true;
    } else {
        auto* table = reinterpret_cast<MIB_TCPTABLE*>(bytes.data());
        if (GetTcpTable(table, &size, false) != NO_ERROR) return false;
        for (DWORD i = 0; i < table->dwNumEntries; ++i) if (table->table[i].dwState == MIB_TCP_STATE_LISTEN &&
            ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == port) return true;
    }
    return false;
}
}

class LiveProtocolServer::Impl {
public:
    Impl(PlaybackProtocol protocol, unsigned int count, std::chrono::seconds lifetime, unsigned int frame_rate, std::chrono::milliseconds video_delay)
        : protocol_(protocol), count_(count), deadline_(Clock::now() + lifetime), frame_rate_(frame_rate), video_delay_(video_delay)
    {
        require(protocol == PlaybackProtocol::Rtmp || protocol == PlaybackProtocol::Srt, "fixture protocol invalid");
        require(frame_rate > 0 && frame_rate <= 60, "fixture frame rate invalid");
        require(video_delay >= 0ms && video_delay <= 2s,"fixture video delay invalid");
        const SOCKET reserved = socket(AF_INET, protocol == PlaybackProtocol::Srt ? SOCK_DGRAM : SOCK_STREAM, 0);
        require(reserved != INVALID_SOCKET, "fixture socket failed");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        const bool bound = bind(reserved, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
            getsockname(reserved, reinterpret_cast<sockaddr*>(&address), &length) == 0;
        port_ = ntohs(address.sin_port); closesocket(reserved);
        require(bound, "fixture ephemeral port unavailable");
        prepareMedia();
        worker_ = std::thread([this] { serve(); });
        const auto end = Clock::now() + 2s;
        while (Clock::now() < end && !failed_ && !listening(port_, protocol == PlaybackProtocol::Srt)) std::this_thread::sleep_for(5ms);
        if (failed_ || !listening(port_, protocol == PlaybackProtocol::Srt)) {
            stopping_ = true; worker_.join(); throw std::runtime_error("FFmpeg fixture listener did not bind");
        }
    }
    ~Impl() { stopping_ = true; if (worker_.joinable()) worker_.join(); }
    std::string url() const { return std::string(protocol_ == PlaybackProtocol::Rtmp ? "rtmp://127.0.0.1:" : "srt://127.0.0.1:") +
        std::to_string(port_) + (protocol_ == PlaybackProtocol::Rtmp ? "/live/fixture" : ""); }
    void pause(bool value) { paused_ = value; }
    void disconnect() { ++revision_; }
    unsigned int accepted() const { return accepted_; }
    unsigned int active() const { return active_; }

private:
    struct Attempt {
        Impl* owner;
        unsigned int revision;
        static int interrupted(void* opaque) {
            const auto& at = *static_cast<Attempt*>(opaque);
            return at.owner->stopping_ || at.owner->revision_ != at.revision || Clock::now() >= at.owner->deadline_;
        }
    };
    void prepareMedia()
    {
        // 与 RTSP 夹具同源的 32x32 Baseline SPS/PPS/IDR，Annex B 可由 FLV/TS muxer 正常封装。
        for (const auto* encoded : {"Z0LACtolsBEAAAMAAQAAAwAyjxImoA==", "aM4PyA=="}) {
            unsigned char decoded[64]; const int n = av_base64_decode(decoded, encoded, sizeof(decoded));
            require(n > 0, "fixture SPS/PPS invalid");
            extra_.insert(extra_.end(), {0,0,0,1}); extra_.insert(extra_.end(), decoded, decoded+n);
        }
        video_ = extra_;
        video_.insert(video_.end(), {0,0,0,1,0x65,0x88,0x84,0x3a,0x26,0x28,0,8,0xf6,0x49,0xd7,0x5e});
        Encoder e; const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        require(encoder != nullptr, "fixture requires FFmpeg AAC encoder");
        e.codec = avcodec_alloc_context3(encoder); require(e.codec != nullptr, "fixture encoder allocation failed");
        e.codec->sample_rate = 48000; e.codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
        e.codec->bit_rate = 32000; e.codec->time_base = {1,48000}; e.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_channel_layout_default(&e.codec->ch_layout, 1);
        require(avcodec_open2(e.codec, encoder, nullptr) >= 0, "fixture AAC encoder unavailable");
        audio_extra_.assign(e.codec->extradata, e.codec->extradata + e.codec->extradata_size);
        e.frame = av_frame_alloc(); e.packet = av_packet_alloc(); require(e.frame && e.packet, "fixture frame allocation failed");
        e.frame->format = e.codec->sample_fmt; e.frame->sample_rate = 48000; e.frame->nb_samples = e.codec->frame_size;
        av_channel_layout_copy(&e.frame->ch_layout, &e.codec->ch_layout);
        require(av_frame_get_buffer(e.frame, 0) >= 0, "fixture audio buffer failed");
        for (int i = 0; i < 5; ++i) {
            require(av_frame_make_writable(e.frame) >= 0, "fixture writable audio failed");
            std::memset(e.frame->data[0], 0, e.frame->nb_samples * sizeof(float));
            e.frame->pts = i * e.frame->nb_samples;
            require(avcodec_send_frame(e.codec, e.frame) >= 0, "fixture AAC encode failed");
            while (avcodec_receive_packet(e.codec, e.packet) >= 0) {
                audio_.assign(e.packet->data, e.packet->data + e.packet->size); av_packet_unref(e.packet);
            }
        }
        require(!audio_.empty(), "fixture AAC encoder produced no packet");
    }
    void stream(Attempt& attempt)
    {
        Output out;
        require(avformat_alloc_output_context2(&out.format, nullptr, protocol_ == PlaybackProtocol::Rtmp ? "flv" : "mpegts", nullptr) >= 0 &&
            out.format && out.packet, "fixture output allocation failed");
        auto* video = avformat_new_stream(out.format, nullptr); auto* audio = avformat_new_stream(out.format, nullptr);
        require(video && audio, "fixture output streams failed");
        // 同一编码按容器时间戳采用 10/25 fps，低帧率会拉长默认帧率分析并暴露探测包突发。
        video->time_base = {1,static_cast<int>(frame_rate_)}; video->avg_frame_rate = {static_cast<int>(frame_rate_),1};
        video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; video->codecpar->codec_id = AV_CODEC_ID_H264;
        video->codecpar->width = video->codecpar->height = 32; video->codecpar->format = AV_PIX_FMT_YUV420P;
        audio->time_base = {1,48000}; audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audio->codecpar->codec_id = AV_CODEC_ID_AAC; audio->codecpar->sample_rate = 48000;
        av_channel_layout_default(&audio->codecpar->ch_layout, 1);
        const auto copy_extra = [](AVCodecParameters* p, const std::vector<unsigned char>& bytes) {
            p->extradata = static_cast<unsigned char*>(av_mallocz(bytes.size()+AV_INPUT_BUFFER_PADDING_SIZE));
            require(p->extradata != nullptr, "fixture extradata allocation failed");
            std::copy(bytes.begin(), bytes.end(), p->extradata); p->extradata_size = static_cast<int>(bytes.size());
        };
        copy_extra(video->codecpar, extra_); copy_extra(audio->codecpar, audio_extra_);
        out.format->interrupt_callback = {Attempt::interrupted, &attempt};
        // 延后首视频的用例仍持续送音频，避免测试 muxer 自身等待视频而制造额外探测突发。
        out.format->max_interleave_delta = 50000;
        AVDictionary* options = nullptr;
        av_dict_set(&options, protocol_ == PlaybackProtocol::Rtmp ? "listen" : "mode",
            protocol_ == PlaybackProtocol::Rtmp ? "1" : "listener", 0);
        if (protocol_ == PlaybackProtocol::Srt) { av_dict_set(&options, "transtype", "live", 0); av_dict_set(&options, "latency", "120000", 0); }
        const auto address = url();
        const int opened = avio_open2(&out.format->pb, address.c_str(), AVIO_FLAG_WRITE, &out.format->interrupt_callback, &options);
        av_dict_free(&options);
        if (opened < 0) return;
        ++accepted_; ++active_;
        const struct Active { std::atomic<unsigned int>& value; ~Active() { --value; } } active{active_};
        while (paused_ && !Attempt::interrupted(&attempt)) std::this_thread::sleep_for(5ms);
        if (Attempt::interrupted(&attempt) || avformat_write_header(out.format, nullptr) < 0) return;
        auto start = Clock::now(); auto pause_at = start;
        std::int64_t video_frame = static_cast<std::int64_t>(frame_rate_)*video_delay_.count()/1000, audio_frame = 0;
        while (!Attempt::interrupted(&attempt)) {
            if (paused_) { const auto now = Clock::now(); start += now-pause_at; pause_at = now; std::this_thread::sleep_for(5ms); continue; }
            pause_at = Clock::now();
            const bool is_audio = audio_frame * frame_rate_ <= video_frame * 48000;
            const auto pts = is_audio ? audio_frame : video_frame;
            const AVRational time_base = is_audio ? AVRational{1,48000} : AVRational{1,static_cast<int>(frame_rate_)};
            const auto due = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(pts * av_q2d(time_base)));
            if (Clock::now() < due) { std::this_thread::sleep_for(2ms); continue; }
            const auto& bytes = is_audio ? audio_ : video_;
            if (av_new_packet(out.packet, static_cast<int>(bytes.size())) < 0) break;
            std::copy(bytes.begin(), bytes.end(), out.packet->data);
            out.packet->stream_index = is_audio ? audio->index : video->index;
            out.packet->pts = out.packet->dts = pts; out.packet->duration = is_audio ? 1024 : 1;
            out.packet->flags = AV_PKT_FLAG_KEY;
            av_packet_rescale_ts(out.packet, time_base, is_audio ? audio->time_base : video->time_base);
            const int written = av_interleaved_write_frame(out.format, out.packet); av_packet_unref(out.packet);
            if (written < 0) break;
            if (is_audio) audio_frame += 1024; else ++video_frame;
        }
    }
    void serve()
    {
        try {
            for (unsigned int i=0; i<count_ && !stopping_ && Clock::now()<deadline_; ++i) {
                Attempt attempt{this, revision_.load()}; stream(attempt);
            }
        } catch (...) { failed_ = true; }
    }
    Winsock winsock_;
    PlaybackProtocol protocol_;
    unsigned int count_;
    Clock::time_point deadline_;
    unsigned int frame_rate_;
    std::chrono::milliseconds video_delay_;
    unsigned short port_ = 0;
    std::vector<unsigned char> extra_, video_, audio_extra_, audio_;
    std::atomic<bool> stopping_{false}, paused_{false}, failed_{false};
    std::atomic<unsigned int> revision_{0}, accepted_{0}, active_{0};
    std::thread worker_;
};

LiveProtocolServer::LiveProtocolServer(PlaybackProtocol protocol, unsigned int connections, std::chrono::seconds lifetime,
    unsigned int video_frame_rate, std::chrono::milliseconds video_start_delay)
    : impl_(std::make_unique<Impl>(protocol, connections, lifetime, video_frame_rate, video_start_delay)) {}
LiveProtocolServer::~LiveProtocolServer() = default;
std::string LiveProtocolServer::url() const { return impl_->url(); }
void LiveProtocolServer::setMediaPaused(bool paused) noexcept { impl_->pause(paused); }
void LiveProtocolServer::disconnectCurrent() noexcept { impl_->disconnect(); }
unsigned int LiveProtocolServer::acceptedConnections() const noexcept { return impl_->accepted(); }
unsigned int LiveProtocolServer::activeConnections() const noexcept { return impl_->active(); }
