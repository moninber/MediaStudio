#include "MediaDecoder.h"
#include "PlaybackSnapshot.h"
#include "RtspBlackhole.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Release 构建同样执行断言，并显示失败的实际判据。
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// 验证类型不由扩展名决定，直播和点播不共享暂停、EOF、回压或恢复位置策略。
void testSourcePolicies()
{
    const std::string local_path = u8"E:\\媒体\\测试 sample.mp4";
    const auto file = PlaybackSource::localFile(local_path);
    const auto unc = PlaybackSource::localFile(u8"\\\\server\\共享\\clip.mp4");
    const auto live = PlaybackSource::networkLive("RTSP://user:secret@example.test/live?token=secret");
    const auto fast = PlaybackSource::networkLive("rtsp://example.test/live",
                                                  {RtspTransport::Udp, LivePreset::LowLatency, false});
    const std::string vod_url = "https://example.test/download?signature=a%2Fb";
    const auto vod = PlaybackSource::networkVod(vod_url);
    require(file.kind() == PlaybackSourceKind::LocalFile && file.protocol() == PlaybackProtocol::File &&
            file.isValid() && file.address() == local_path && file.displayName() == u8"测试 sample.mp4" &&
            file.liveOptions() == nullptr, "local path/display contract");
    require(unc.isValid() && unc.address() == u8"\\\\server\\共享\\clip.mp4", "UNC path was changed");
    require(live.kind() == PlaybackSourceKind::NetworkLive && live.protocol() == PlaybackProtocol::Rtsp &&
            live.isValid() && live.displayName() == "RTSP", "RTSP source must not expose the raw URL");
    require(live.liveOptions() && live.liveOptions()->rtsp_transport == RtspTransport::Tcp &&
            live.liveOptions()->preset == LivePreset::Stable && live.liveOptions()->auto_reconnect,
            "default live options");
    require(fast.liveOptions() && fast.liveOptions()->rtsp_transport == RtspTransport::Udp &&
            fast.liveOptions()->preset == LivePreset::LowLatency && !fast.liveOptions()->auto_reconnect,
            "explicit live options");
    require(PlaybackSource::networkLive("rtmp://example.test/live").isValid() &&
            PlaybackSource::networkLive("srt://example.test:9002?mode=caller").isValid(), "live protocols");
    require(vod.kind() == PlaybackSourceKind::NetworkVod && vod.protocol() == PlaybackProtocol::Https &&
            vod.isValid() && vod.address() == vod_url && vod.displayName() == "HTTPS" &&
            vod.liveOptions() == nullptr, "signed VoD URL and display name");
    require(!PlaybackSource::localFile("https://example.test/clip.mp4").isValid() &&
            !PlaybackSource::networkLive("https://example.test/clip.mp4").isValid() &&
            !PlaybackSource::networkVod("rtsp://example.test/clip.mp4").isValid() &&
            !PlaybackSource::networkVod("https://user:secret@example.test/clip.mp4").isValid(),
            "wrong source type or unsupported VoD authentication");

    const auto local_policy = playbackPolicy(file);
    const auto live_policy = playbackPolicy(live);
    const auto vod_policy = playbackPolicy(vod);
    require(local_policy.pause == PauseBehavior::KeepPosition &&
            local_policy.end_of_stream == EndOfStreamBehavior::Drain &&
            local_policy.queue_full == QueueFullBehavior::LegacyThrottle &&
            local_policy.reconnect_position == ReconnectPosition::None &&
            local_policy.open_timeout == 0ms && local_policy.probe_timeout == 0ms &&
            local_policy.read_timeout == 0ms, "local V1 policy");
    require(live_policy.pause == PauseBehavior::DisconnectLive &&
            live_policy.end_of_stream == EndOfStreamBehavior::RecoverLive &&
            live_policy.queue_full == QueueFullBehavior::ReconnectLive &&
            live_policy.reconnect_position == ReconnectPosition::LiveEdge &&
            live_policy.open_timeout == 5s && live_policy.probe_timeout == 5s &&
            live_policy.read_timeout == 5s && playbackPolicy(fast).read_timeout == 3s, "live policy");
    require(vod_policy.pause == PauseBehavior::FreezeVod &&
            vod_policy.end_of_stream == EndOfStreamBehavior::Drain &&
            vod_policy.queue_full == QueueFullBehavior::Backpressure &&
            vod_policy.reconnect_position == ReconnectPosition::LastPlayedPosition &&
            vod_policy.open_timeout == 10s && vod_policy.probe_timeout == 10s &&
            vod_policy.read_timeout == 10s, "VoD policy");
}

// 时长和定位能力分别验证，快照副本保留原请求、媒体、统计和错误。
void testCapabilitiesAndSnapshot()
{
    const auto local = initialPlaybackCapabilities(PlaybackSourceKind::LocalFile, false);
    const auto live = initialPlaybackCapabilities(PlaybackSourceKind::NetworkLive, true);
    const auto vod = initialPlaybackCapabilities(PlaybackSourceKind::NetworkVod, true);
    require(!local.duration_known && local.can_seek && local.can_change_speed && !local.can_return_to_live,
            "unknown file duration must preserve V1 controls");
    require(!live.duration_known && !live.can_seek && !live.can_change_speed &&
            live.can_return_to_live && !live.can_resume_at_position, "live capabilities");
    require(vod.duration_known && !vod.can_seek && !vod.can_change_speed &&
            !vod.can_return_to_live && !vod.can_resume_at_position, "known VoD duration cannot unlock unprobed capabilities");

    PlaybackSnapshot published;
    require(published.state == PlaybackState::Idle && !published.last_error && !published.network_stats,
            "initial snapshot is idle and has no network data");
    published.request_id = 7;
    published.connection_attempt_id = 11;
    published.source_kind = PlaybackSourceKind::NetworkLive;
    published.display_name = "RTSP";
    published.state = PlaybackState::Reconnecting;
    published.capabilities = live;
    published.media_info.audio_info.codec_name = "pcm_mulaw";
    published.stats.audio_queue_packets = 3;
    published.last_error = MediaReadResult{MediaReadStatus::Timeout, AVERROR_EXIT};
    published.network_stats.emplace();
    require(!published.network_stats->application_bitrate_bps && !published.network_stats->last_packet_age_seconds,
            "unsampled network metrics must be absent");
    published.network_stats->last_packet_age_seconds = 1.5;
    const PlaybackSnapshot saved = published;
    published.request_id = 8;
    published.connection_attempt_id = 12;
    published.source_kind = PlaybackSourceKind::LocalFile;
    published.display_name = "another.wav";
    published.state = PlaybackState::Opening;
    published.capabilities = local;
    published.media_info.audio_info.codec_name.clear();
    published.stats.audio_queue_packets = 0;
    published.last_error.reset();
    published.network_stats.reset();
    require(saved.request_id == 7 && saved.connection_attempt_id == 11 &&
            saved.source_kind == PlaybackSourceKind::NetworkLive && saved.display_name == "RTSP" &&
            saved.state == PlaybackState::Reconnecting && saved.capabilities.can_return_to_live &&
            saved.media_info.audio_info.codec_name == "pcm_mulaw" && saved.stats.audio_queue_packets == 3 &&
            saved.network_stats && saved.network_stats->last_packet_age_seconds == 1.5,
            "published snapshot was changed through another copy");
    const auto* error = saved.last_error ? std::get_if<MediaReadResult>(&*saved.last_error) : nullptr;
    require(error && error->status == MediaReadStatus::Timeout && error->ffmpegError == AVERROR_EXIT,
            "snapshot preserves typed error");
}

// 分别验证暂时无包、正常读尽、传输超时、中断原因和无法分类的原始错误。
void testErrorClassification()
{
    struct Case { int code; MediaIoStopReason reason; MediaReadStatus expected; };
    const Case cases[]{
        {0, MediaIoStopReason::None, MediaReadStatus::Packet},
        {AVERROR(EAGAIN), MediaIoStopReason::None, MediaReadStatus::Again},
        {AVERROR_EOF, MediaIoStopReason::None, MediaReadStatus::EndOfStream},
        {AVERROR(ETIMEDOUT), MediaIoStopReason::None, MediaReadStatus::Timeout},
        {AVERROR_EXIT, MediaIoStopReason::Timeout, MediaReadStatus::Timeout},
        {AVERROR_EXIT, MediaIoStopReason::Cancelled, MediaReadStatus::Cancelled},
        {AVERROR(ETIMEDOUT), MediaIoStopReason::Cancelled, MediaReadStatus::Cancelled},
        {AVERROR_EXIT, MediaIoStopReason::None, MediaReadStatus::Error},
        {AVERROR(EIO), MediaIoStopReason::None, MediaReadStatus::Error}
    };
    for (const auto& entry : cases) {
        const auto result = classifyMediaReadResult(entry.code, entry.reason);
        require(result.status == entry.expected && result.ffmpegError == entry.code,
                "read error classification or raw code mismatch");
    }
    MediaDecoder decoder;
    const auto invalid = decoder.open(PlaybackSource::localFile("rtsp://example.test/live"));
    require(invalid.status == MediaOpenStatus::Error && invalid.stage == MediaOpenStage::Validation &&
            invalid.ffmpegError == AVERROR(EINVAL), "invalid source must fail before I/O");
    const auto reserved = decoder.open(PlaybackSource::networkVod("https://example.test/clip.mp4"));
    require(reserved.status == MediaOpenStatus::Error && reserved.stage == MediaOpenStage::Validation &&
            reserved.ffmpegError == AVERROR(ENOSYS), "VoD is not enabled in 1.1");
}

using Packet = std::unique_ptr<AVPacket, void(*)(AVPacket*)>;

// AVPacket 结构和其负载都由 FFmpeg 的释放函数回收。
Packet makePacket()
{
    Packet packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    require(packet != nullptr, "could not allocate AVPacket");
    return packet;
}

// 生成 1 秒单声道 PCM WAV，避免普通 CTest 依赖用户素材或 FFmpeg 命令行。
class TemporaryWave {
public:
    TemporaryWave()
    {
        const auto temp = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int i = 0; i < 8; ++i) {
            const auto candidate = temp / ("mediastudio-ctest-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                directory_ = candidate;
                break;
            }
        }
        require(!directory_.empty(), "could not create temporary fixture directory");
        try {
            std::ofstream output(path(), std::ios::binary);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            const auto little = [&output](std::uint32_t value, unsigned count) {
                for (unsigned i = 0; i < count; ++i) output.put(static_cast<char>(value >> (8 * i)));
            };
            output << "RIFF"; little(16036, 4); output << "WAVEfmt "; little(16, 4);
            little(1, 2); little(1, 2); little(8000, 4); little(16000, 4); little(2, 2); little(16, 2);
            output << "data"; little(16000, 4);
            const std::string samples(16000, '\0');
            output.write(samples.data(), samples.size());
            output.close();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~TemporaryWave() { cleanup(); }
    std::filesystem::path path() const { return directory_ / "local fixture.wav"; }

private:
    // 只删除夹具创建的文件和空目录。
    void cleanup() noexcept
    {
        std::error_code error;
        std::filesystem::remove(path(), error);
        std::filesystem::remove(directory_, error);
    }

    std::filesystem::path directory_;
};

// 旧文件入口与新结果接口一起验证：读到音频、真实 EOF，seek 后重新读到包。
void testLocalFile()
{
    const TemporaryWave wave;
    const auto path = wave.path().u8string();
    auto io = std::make_shared<MediaIoControl>();
    io->cancel();
    MediaDecoder decoder;
    const auto cancelled = decoder.open(PlaybackSource::localFile(path), io);
    require(cancelled.status == MediaOpenStatus::Cancelled && cancelled.ffmpegError == AVERROR_EXIT &&
            io->isCancelled(), "pre-cancelled open must preserve cancellation");
    require(decoder.open(path), "legacy open did not recover from prior cancelled attempt");
    require(decoder.hasAudio() && !decoder.hasVideo() && decoder.getMediaInfo().file_path == path &&
            decoder.getMediaInfo().duration > 0.9 && decoder.getMediaInfo().duration < 1.1,
            "local WAV metadata");
    auto packet = makePacket();
    require(decoder.readPacket(packet.get()) && decoder.isAudioPacket(packet.get()), "legacy local read");
    av_packet_unref(packet.get());
    const auto opened = decoder.open(PlaybackSource::localFile(path));
    require(opened.status == MediaOpenStatus::Opened && opened.ffmpegError == 0, "typed local open");
    int packets = 0;
    bool reached_eof = false;
    for (int i = 0; i < 100; ++i) {
        const auto result = decoder.readPacketResult(packet.get());
        if (result.status == MediaReadStatus::EndOfStream) {
            reached_eof = result.ffmpegError == AVERROR_EOF;
            break;
        }
        require(result.status == MediaReadStatus::Packet && decoder.isAudioPacket(packet.get()),
                "unexpected local read result");
        ++packets;
        av_packet_unref(packet.get());
    }
    require(packets > 0 && reached_eof, "did not read local audio through real EOF");
    decoder.seek(0.5);
    require(decoder.readPacket(packet.get()) && decoder.isAudioPacket(packet.get()), "read after local seek");
}

// 可选地对用户的 MP4 做真实音视频读包和 seek，不把外部路径写入 CTest。
void testExternalLocalFile(const std::string& path)
{
    MediaDecoder decoder;
    require(decoder.open(path), "could not open external local sample");
    const auto& info = decoder.getMediaInfo();
    require(info.has_audio && info.has_video && info.duration > 0.0, "expected an A/V sample with known duration");
    auto packet = makePacket();
    bool audio = false, video = false;
    for (int i = 0; i < 1024 && !(audio && video); ++i) {
        require(decoder.readPacket(packet.get()), "external sample stopped before both tracks were read");
        audio = audio || decoder.isAudioPacket(packet.get());
        video = video || decoder.isVideoPacket(packet.get());
        av_packet_unref(packet.get());
    }
    require(audio && video, "external sample did not yield both track types");
    decoder.seek(info.duration / 2);
    require(decoder.readPacket(packet.get()) && (decoder.isAudioPacket(packet.get()) || decoder.isVideoPacket(packet.get())),
            "external local read after seek");
}

// 只在同一次读持续 200 ms 未返回时才从另一线程取消，避免把快速读包误判为阻塞。
class ReadCanceller {
public:
    explicit ReadCanceller(MediaIoControl& control)
        : worker_([this, &control] { cancelWhenBlocked(control); })
    {
    }

    ~ReadCanceller() { finish(); }

    void beginRead()
    {
        std::lock_guard lock(mutex_);
        reading_ = true;
        ++sequence_;
        changed_.notify_one();
    }

    void endRead()
    {
        std::lock_guard lock(mutex_);
        reading_ = false;
        changed_.notify_one();
    }

    std::optional<Clock::time_point> finish()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            changed_.notify_one();
        }
        if (worker_.joinable()) worker_.join();
        return cancelled_at_;
    }

private:
    void cancelWhenBlocked(MediaIoControl& control)
    {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            changed_.wait(lock, [&] { return stopping_ || reading_; });
            if (stopping_) return;
            const auto sequence = sequence_;
            if (!changed_.wait_for(lock, 200ms, [&] { return stopping_ || !reading_ || sequence_ != sequence; })) {
                cancelled_at_ = Clock::now();
                control.cancel();
                return;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    bool reading_ = false;
    bool stopping_ = false;
    unsigned sequence_ = 0;
    std::optional<Clock::time_point> cancelled_at_;
    std::thread worker_;
};

// 先通过真实 RTSP/TCP 打开和读包，再等待不主动断开的连接触发两种退出原因。
void testRtspBlackhole(bool cancel)
{
    RtspBlackhole server;
    auto address = server.url();
    // 超时用例同时覆盖 URL scheme 大小写兼容，夹具仍使用真实 TCP 连接。
    if (!cancel) address.replace(0, 4, "RTSP");
    const auto source = PlaybackSource::networkLive(address, {RtspTransport::Tcp, LivePreset::LowLatency});
    auto control = std::make_shared<MediaIoControl>();
    MediaDecoder decoder;
    const auto opened = decoder.open(source, control);
    require(opened.status == MediaOpenStatus::Opened && decoder.hasAudio(), "RTSP open/probe did not yield audio");
    require(decoder.getMediaInfo().file_path.empty() && decoder.getMediaInfo().file_name == "RTSP" &&
            source.address() == address, "network open exposed or changed the source address");
    auto packet = makePacket();
    require(decoder.readPacketResult(packet.get()).status == MediaReadStatus::Packet &&
            decoder.isAudioPacket(packet.get()), "initial RTSP audio packet was not delivered");
    av_packet_unref(packet.get());

    auto canceller = cancel ? std::make_unique<ReadCanceller>(*control) : nullptr;
    for (int i = 0; i < 256; ++i) {
        const auto started = Clock::now();
        if (canceller) canceller->beginRead();
        const auto result = decoder.readPacketResult(packet.get());
        const auto ended = Clock::now();
        if (canceller) canceller->endRead();
        if (result.status == MediaReadStatus::Packet || result.status == MediaReadStatus::Again) {
            av_packet_unref(packet.get());
            continue;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ended - started);
        std::cout << "RTSP terminal read status=" << static_cast<int>(result.status) << " raw="
                  << result.ffmpegError << " elapsed_ms=" << elapsed.count() << '\n';
        require(server.isHoldingConnection(), "fixture disconnected before FFmpeg returned");
        require(result.ffmpegError < 0, "terminal read did not preserve a negative FFmpeg return");
        const auto timeout = playbackPolicy(source).read_timeout;
        if (canceller) {
            const auto cancelled_at = canceller->finish();
            require(result.status == MediaReadStatus::Cancelled && control->isCancelled(),
                    "blocked read did not report cancellation");
            require(cancelled_at && elapsed >= 200ms && *cancelled_at <= ended &&
                    ended - *cancelled_at <= 2s && elapsed < timeout, "cancellation did not interrupt the active read");
            std::cout << "RTSP cancel_to_return_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(ended - *cancelled_at).count()
                      << '\n';
        } else {
            require(result.status == MediaReadStatus::Timeout && !control->isCancelled(),
                    "blocked read did not report a deadline");
            require(elapsed >= timeout - 100ms && elapsed <= timeout + 2s, "blocked read missed its deadline");
        }
        // 有界 TEARDOWN 只允许关闭写入，不能清除上层共享的粘性取消状态。
        decoder.close();
        require(control->isCancelled() == cancel, "decoder close reset or invented the caller's cancellation state");
        return;
    }
    throw std::runtime_error("no blocked RTSP read after draining fixture packets");
}

// 用同一个稳定模式入口校验原始地址，并观察 TCP/UDP SETUP 和相应的 RTP 数据。
void testStableRtsp()
{
    const std::string address = "RTSP://MediaStudioSecretUser:p%40ss@example.test:554/stream?token=MediaStudioSecretQuery";
    const auto accepted = createStableRtspSource(address, RtspTransport::Tcp);
    require(accepted.source && accepted.error == RtspSourceError::None &&
            accepted.source->address() == address && accepted.source->displayName() == "RTSP" &&
            accepted.source->kind() == PlaybackSourceKind::NetworkLive &&
            accepted.source->protocol() == PlaybackProtocol::Rtsp, "stable validation changed or exposed the RTSP address");
    auto unsupported = createStableRtspSource("https://example.test/watch?token=MediaStudioSecretUnsupported",
                                              RtspTransport::Udp);
    require(!unsupported.source && unsupported.error == RtspSourceError::UnsupportedProtocol,
            "stable entry accepted an unsupported protocol");
    for (const std::string_view invalid : {"rtsp:///no-host", "rtsp://example.test:65536/live", "rtsp://example.test/with space"}) {
        const auto rejected = createStableRtspSource(std::string(invalid), RtspTransport::Tcp);
        require(!rejected.source && rejected.error == RtspSourceError::InvalidAddress,
                "stable entry accepted a malformed RTSP address");
    }

    const auto bind_address = std::getenv("MEDIASTUDIO_TEST_RTSP_ADDRESS");
    const std::string local = bind_address ? bind_address : "127.0.0.1";
    for (const auto transport : {RtspTransport::Tcp, RtspTransport::Udp}) {
        using Setup = RtspBlackhole::SetupTransport;
        const auto expected = transport == RtspTransport::Tcp ? Setup::Tcp : Setup::Udp;
        RtspBlackhole server(RtspBlackhole::StallAt::ReadPacket, local, expected);
        auto raw = server.url() + "?token=MediaStudioSecretQuery";
        raw.insert(7, "MediaStudioSecretUser:MediaStudioSecretPassword@");
        const auto validated = createStableRtspSource(raw, transport);
        require(validated.source && validated.source->address() == raw, "fixture credentials or query changed during validation");
        const auto* options = validated.source->liveOptions();
        require(options && options->rtsp_transport == transport && options->preset == LivePreset::Stable &&
                !options->auto_reconnect, "stable transport selection changed or enabled automatic reconnect");

        MediaDecoder decoder;
        const auto opened = decoder.open(*validated.source);
        require(opened.status == MediaOpenStatus::Opened && decoder.hasAudio() && server.setupTransport() == expected,
                "stable RTSP did not negotiate selected RTP transport and probe real audio");
        const auto& info = decoder.getMediaInfo();
        require(info.file_path.empty() && info.file_name == "RTSP", "network credentials reached media metadata");
        auto packet = makePacket();
        require(decoder.readPacketResult(packet.get()).status == MediaReadStatus::Packet && decoder.isAudioPacket(packet.get()),
                "stable RTSP transport negotiated without delivering RTP audio");
        std::cout << "stable_rtsp_setup=" << (transport == RtspTransport::Tcp ? "TCP" : "UDP")
                  << " rtp_audio_packet=1\n";
    }

    // RTSP 403 诊断可携带原始请求目标。CTest 另外检查 stderr 中没有测试凭据与查询参数。
    RtspBlackhole rejecting(RtspBlackhole::StallAt::RejectOpen, local);
    const auto private_url = rejecting.url() + "?token=MediaStudioSecretRejected";
    const auto private_source = createStableRtspSource(private_url, RtspTransport::Tcp);
    require(private_source.source.has_value(), "could not construct rejected private RTSP request");
    MediaDecoder decoder;
    const auto result = decoder.open(*private_source.source);
    require(result.status == MediaOpenStatus::Error && result.stage == MediaOpenStage::OpenInput &&
            result.ffmpegError == AVERROR_HTTP_FORBIDDEN,
            "private URL rejection did not reach the FFmpeg RTSP open failure path");
    std::cout << "stable_rtsp_rejected_safe=1\n";
}

} // namespace

// 每个 CTest 启动一个进程，夹具或解码器异常时仍由外层超时保护。
int main(int argc, char* argv[])
{
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        if (mode == "contracts" && argc == 2) {
            testSourcePolicies();
            testCapabilitiesAndSnapshot();
            testErrorClassification();
        } else if (mode == "local-file" && argc == 2) {
            testLocalFile();
        } else if (mode == "external-local" && argc == 3) {
            testExternalLocalFile(argv[2]);
        } else if (mode == "rtsp-cancel" && argc == 2) {
            testRtspBlackhole(true);
        } else if (mode == "rtsp-timeout" && argc == 2) {
            testRtspBlackhole(false);
        } else if (mode == "rtsp-stable" && argc == 2) {
            testStableRtsp();
        } else {
            throw std::runtime_error("expected contracts, local-file, external-local <path>, rtsp-cancel, rtsp-timeout or rtsp-stable");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Playback test failed: " << error.what() << '\n';
        return 1;
    }
}
