#include "PlayerController.h"
#include "RtspBlackhole.h"
#include "VideoWidget.h"

#include <QApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Release 测试也报告失败条件，便于定位命令、收包或显示归属问题。
void require(bool condition, const char* detail)
{
    if (!condition) throw std::runtime_error(detail);
}

// 在有界期限内观察跨线程状态，避免依赖固定的调度时序。
template<typename Predicate>
void waitUntil(Predicate ready, Clock::duration timeout, const char* detail)
{
    const auto deadline = Clock::now() + timeout;
    do {
        if (ready()) return;
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < deadline);
    require(ready(), detail);
}

// 由 FFmpeg 生成带索引的纯视频 AVI，定位和帧测试不依赖素材下载、声卡或命令行编码器。
void writeVideo(const std::filesystem::path& path, int width)
{
    constexpr int height = 48;
    constexpr int frame_count = 135;
    constexpr AVRational time_base{1, 15};
    struct Output {
        AVFormatContext* context = nullptr;
        AVPacket* packet = nullptr;

        // 封装或写盘失败也会关闭文件并释放 FFmpeg 对象。
        ~Output()
        {
            av_packet_free(&packet);
            if (context) {
                if (context->pb) avio_closep(&context->pb);
                avformat_free_context(context);
            }
        }
    } output;

    const auto filename = path.u8string();
    require(avformat_alloc_output_context2(&output.context, nullptr, "avi", filename.c_str()) >= 0 &&
            output.context, "could not allocate AVI output");
    auto* stream = avformat_new_stream(output.context, nullptr);
    require(stream, "could not allocate AVI stream");
    stream->time_base = time_base;
    stream->avg_frame_rate = AVRational{15, 1};
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    stream->codecpar->codec_tag = MKTAG('I', '4', '2', '0');
    stream->codecpar->format = AV_PIX_FMT_YUV420P;
    stream->codecpar->width = width;
    stream->codecpar->height = height;
    require(avio_open(&output.context->pb, filename.c_str(), AVIO_FLAG_WRITE) >= 0, "could not create AVI fixture");
    require(avformat_write_header(output.context, nullptr) >= 0, "could not write AVI header");

    output.packet = av_packet_alloc();
    require(output.packet, "could not allocate fixture packet");
    const int luma_size = width * height;
    const int frame_size = luma_size * 3 / 2;
    for (int frame = 0; frame < frame_count; ++frame) {
        require(av_new_packet(output.packet, frame_size) >= 0, "could not allocate raw frame");
        // 亮度随帧号变化，两个文件使用不同分辨率核对切源后信息是否串线。
        std::fill_n(output.packet->data, luma_size, static_cast<uint8_t>(32 + frame));
        std::fill_n(output.packet->data + luma_size, frame_size - luma_size, uint8_t{128});
        output.packet->pts = frame;
        output.packet->dts = frame;
        output.packet->duration = 1;
        output.packet->stream_index = stream->index;
        output.packet->flags = AV_PKT_FLAG_KEY;
        av_packet_rescale_ts(output.packet, time_base, stream->time_base);
        require(av_interleaved_write_frame(output.context, output.packet) >= 0, "could not write raw frame");
    }
    require(av_write_trailer(output.context) >= 0, "could not finish AVI fixture");
}

// 每次测试在 CMake 指定目录下创建私有夹具目录，只回收自己创建的文件和空目录。
class VideoFixtures {
public:
    VideoFixtures()
    {
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto candidate = std::filesystem::absolute("mediastudio-session-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                directory_ = candidate;
                break;
            }
        }
        require(!directory_.empty(), "could not create private fixture directory");
        try {
            writeVideo(first(), 64);
            writeVideo(second(), 80);
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~VideoFixtures() { cleanup(); }
    VideoFixtures(const VideoFixtures&) = delete;
    VideoFixtures& operator=(const VideoFixtures&) = delete;

    // 提供可区分的两份本地媒体和一个不存在的路径。
    std::filesystem::path first() const { return directory_ / "first.avi"; }
    std::filesystem::path second() const { return directory_ / "second.avi"; }
    std::filesystem::path missing() const { return directory_ / "missing.avi"; }

private:
    // 使用 error_code 处理清理失败，析构不会覆盖原测试异常。
    void cleanup() noexcept
    {
        std::error_code ignored;
        std::filesystem::remove(first(), ignored);
        std::filesystem::remove(second(), ignored);
        std::filesystem::remove(directory_, ignored);
    }

    std::filesystem::path directory_;
};

// 核对包引用随原标签移动，重置能丢弃队列中的旧包但不会篡改已经出队的包。
void testPacketEpochs()
{
    // FFmpeg 包和队列持有的引用独立收尾，断言失败不会泄露已分配数据。
    const auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
    std::unique_ptr<AVPacket, decltype(free_packet)> input(av_packet_alloc(), free_packet);
    std::unique_ptr<AVPacket, decltype(free_packet)> output(av_packet_alloc(), free_packet);
    require(input && output, "could not allocate packet for queue test");
    require(av_new_packet(input.get(), 1) >= 0, "could not allocate bounded packet payload");
    input->data[0] = 11;
    input->pts = 123;

    PacketQueue queue;
    const PlaybackEpoch first{5, 9, 1};
    const PlaybackEpoch obsolete{5, 9, 2};
    const PlaybackEpoch latest{6, 10, 3};
    require(queue.push(input.get(), first) && queue.push(input.get(), obsolete), "could not queue tagged packets");
    av_packet_unref(input.get());
    PlaybackEpoch popped;
    require(queue.pop(output.get(), popped, false) && popped == first &&
            output->pts == 123 && output->size == 1 && output->data[0] == 11,
            "popped packet did not retain the enqueued payload and tag");

    queue.reset();
    require(queue.size() == 0 && !queue.pop(input.get(), popped, false) && popped == first &&
            output->data[0] == 11, "reset modified in-flight work or preserved a queued old packet");
    require(queue.push(output.get(), latest), "empty reset queue did not accept the latest epoch");
    queue.finish();
    require(queue.pop(input.get(), popped, false) && popped == latest && input->data[0] == 11 &&
            !queue.pop(output.get(), popped, false), "finish did not drain exactly the latest packet");
    queue.reset();
    require(queue.push(input.get(), latest), "reset after finish failed");
    queue.abort();
    require(queue.isAborted() && queue.size() == 0 && !queue.pop(output.get(), popped, false) &&
            !queue.push(input.get(), latest), "abort did not discard or reject pending work");
}

// 等到目标请求交付真实视频，再核对时钟、媒体信息和本次连接编号。
PlaybackSnapshot waitForVideo(PlayerController& player, std::uint64_t request_id, int width,
                              Clock::duration timeout = 3s)
{
    PlaybackSnapshot snapshot;
    waitUntil([&] {
        snapshot = player.getPlaybackSnapshot();
        return snapshot.request_id == request_id && snapshot.is_playing &&
            snapshot.has_current_video_output && snapshot.current_seconds >= 0.1;
    }, timeout, "latest local playback did not produce fresh video and advance");
    require(snapshot.source_kind == PlaybackSourceKind::LocalFile && snapshot.connection_attempt_id != 0 &&
            snapshot.media_info.has_video && !snapshot.media_info.has_audio &&
            snapshot.media_info.video_info.width == width && !snapshot.last_error,
            "latest local snapshot has incorrect media or a stale error");
    return snapshot;
}

// 走正常命令入口验证失败、切源、暂停定位、停止与同文件重播。
void testControllerLocal()
{
    const VideoFixtures fixtures;
    VideoWidget widget;
    PlayerController player;
    player.setVideoWidget(&widget);

    const auto failed = player.requestOpen(PlaybackSource::localFile(fixtures.missing().u8string()));
    require(failed != 0, "missing file request was not assigned an ID");
    waitUntil([&] {
        const auto snapshot = player.getPlaybackSnapshot();
        const auto* error = snapshot.last_error ? std::get_if<MediaOpenResult>(&*snapshot.last_error) : nullptr;
        return snapshot.request_id == failed && snapshot.state == PlaybackState::Failed &&
            !snapshot.is_playing && !snapshot.has_current_video_output && error &&
            error->status == MediaOpenStatus::Error && error->stage == MediaOpenStage::OpenInput;
    }, 3s, "missing file did not publish its asynchronous open error");

    const auto first = player.requestOpen(PlaybackSource::localFile(fixtures.first().u8string()));
    const auto first_snapshot = waitForVideo(player, first, 64);
    const auto second_source = PlaybackSource::localFile(fixtures.second().u8string());
    const auto second = player.requestOpen(second_source);
    require(failed < first && first < second, "open commands did not get increasing IDs");
    const auto second_snapshot = waitForVideo(player, second, 80);
    require(first_snapshot.request_id == first && first_snapshot.media_info.video_info.width == 64,
            "old snapshot mutated after a source switch");

    player.pause();
    waitUntil([&] {
        const auto snapshot = player.getPlaybackSnapshot();
        return snapshot.request_id == second && snapshot.state == PlaybackState::Paused;
    }, 2s, "local pause was not applied");
    player.seek(3.0);
    PlaybackSnapshot positioned;
    waitUntil([&] {
        positioned = player.getPlaybackSnapshot();
        return positioned.request_id == second && positioned.state == PlaybackState::Paused &&
            positioned.generation > second_snapshot.generation && positioned.current_seconds >= 2.9 &&
            !positioned.has_current_video_output;
    }, 2s, "paused seek did not change generation and invalidate current video output");
    require(positioned.connection_attempt_id == second_snapshot.connection_attempt_id,
            "local seek changed connection identity");
    std::this_thread::sleep_for(100ms);
    const auto still_paused = player.getPlaybackSnapshot();
    require(still_paused.generation == positioned.generation && !still_paused.has_current_video_output,
            "obsolete frames were counted as current while paused");

    player.play();
    const auto resumed = waitForVideo(player, second, 80);
    require(resumed.generation == positioned.generation && resumed.current_seconds >= 2.9,
            "resume produced output for an obsolete seek position");

    // 积累一段预读数据再切换倍速，能区分原位置继续与直接从队列末端开始。
    for (const float speed : std::array{2.0f, 1.0f}) {
        waitUntil([&] { return player.getPlaybackSnapshot().stats.video_queue_packets >= 20; },
                  2s, "video did not prefetch enough packets to test a speed change");
        player.pause();
        const auto before_speed = player.getPlaybackSnapshot();
        require(before_speed.state == PlaybackState::Paused, "pause before speed change was not applied");
        player.setSpeed(speed);

        PlaybackSnapshot speed_changed;
        waitUntil([&] {
            speed_changed = player.getPlaybackSnapshot();
            return speed_changed.request_id == second && speed_changed.state == PlaybackState::Paused &&
                speed_changed.generation > before_speed.generation && !speed_changed.has_current_video_output;
        }, 2s, "paused speed change did not publish a fresh generation");
        require(speed_changed.connection_attempt_id == before_speed.connection_attempt_id &&
                std::abs(speed_changed.current_seconds - before_speed.current_seconds) <= 0.1,
                "changing speed moved the paused cursor or changed the connection");

        player.play();
        const auto speed_resumed = waitForVideo(player, second, 80);
        require(speed_resumed.generation == speed_changed.generation &&
                speed_resumed.current_seconds >= before_speed.current_seconds - 0.1 &&
                speed_resumed.current_seconds <= before_speed.current_seconds + 0.45,
                "playback did not resume near the paused cursor after changing speed");
        std::cout << "speed_resume rate=" << speed << " generation=" << speed_resumed.generation
                  << " before=" << before_speed.current_seconds << " after=" << speed_resumed.current_seconds << '\n';
    }

    const auto stopped = player.stopAsync();
    require(stopped == second, "stop did not report the request being stopped");
    waitUntil([&] {
        const auto snapshot = player.getPlaybackSnapshot();
        return snapshot.request_id == stopped && snapshot.state == PlaybackState::Idle &&
            snapshot.connection_attempt_id == 0 && !snapshot.is_playing && !snapshot.has_current_video_output;
    }, 3s, "stop did not retire active output");
    const auto replay = player.requestOpen(second_source);
    require(replay > stopped, "replay reused a stopped request ID");
    waitForVideo(player, replay, 80);
    player.shutdown();
}

// 先证明 FFmpeg 已进入真实且未被服务端断开的 RTSP 打开，再用新的本地请求中止它。
void testControllerSupersede()
{
    const VideoFixtures fixtures;
    VideoWidget widget;
    RtspBlackhole server(RtspBlackhole::StallAt::OpenInput);
    PlayerController player;
    player.setVideoWidget(&widget);

    const auto source = PlaybackSource::networkLive(server.url());
    const auto started = Clock::now();
    const auto slow = player.requestOpen(source);
    require(slow != 0 && Clock::now() - started < 1s, "opening a network source blocked the calling thread");
    waitUntil([&] { return server.isHoldingConnection(); }, 2s, "RTSP fixture did not receive the open request");
    const auto pending = player.getPlaybackSnapshot();
    require(pending.request_id == slow && pending.state == PlaybackState::Opening &&
            pending.display_name == "RTSP" && !pending.has_current_video_output,
            "RTSP network open did not expose its pending request");

    const auto replacement_started = Clock::now();
    const auto latest = player.requestOpen(PlaybackSource::localFile(fixtures.first().u8string()));
    require(latest > slow && Clock::now() - replacement_started < 1s,
            "requestOpen blocked behind the superseded FFmpeg open");
    waitForVideo(player, latest, 64, 2s);
    waitUntil([&] { return !server.isHoldingConnection(); }, 1s, "superseded RTSP connection was not closed");
    require(Clock::now() - started < playbackPolicy(source).open_timeout, "superseded open reached its deadline");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - replacement_started);
    std::cout << "supersede_to_local_output_ms=" << elapsed.count() << '\n';

    // 新媒体推进期间复核旧取消和完成通知没有改写请求 ID、错误或媒体信息。
    std::this_thread::sleep_for(100ms);
    const auto settled = player.getPlaybackSnapshot();
    require(settled.request_id == latest && settled.has_current_video_output && !settled.last_error &&
            settled.media_info.file_path == fixtures.first().u8string(), "superseded request polluted the latest output");
    player.shutdown();
}

// 直接验证两个异步输出入口在三种标签发生变化时拒绝迟到数据。
void testWidgetEpochs()
{
    VideoWidget widget;
    const PlaybackEpoch first{1, 1, 0};
    const PlaybackEpoch seek{1, 1, 1};
    const PlaybackEpoch reconnect{1, 2, 1};
    const PlaybackEpoch replacement{2, 1, 0};
    const std::array<uint8_t, 16> rgba{};
    const uint8_t* planes[3] = {rgba.data(), nullptr, nullptr};
    const int strides[3] = {8, 0, 0};
    const auto submit = [&](PlaybackEpoch epoch) {
        return widget.setVideoFrame(epoch, VideoFrameFormat::RGBA, planes, strides, true, false, 2, 2);
    };

    widget.setPlaybackEpoch(first, false);
    require(submit(first) && widget.setSpectrumData(first, {0.5f}), "widget rejected initial output");
    widget.setPlaybackEpoch(seek, true);
    require(!submit(first) && !widget.setSpectrumData(first, {0.5f}) &&
            submit(seek) && widget.setSpectrumData(seek, {0.5f}), "widget accepted output from an old seek generation");
    widget.setPlaybackEpoch(reconnect, true);
    require(!submit(seek) && !widget.setSpectrumData(seek, {0.5f}) &&
            submit(reconnect), "widget accepted output from an old connection");
    widget.setPlaybackEpoch(replacement, false);
    require(!submit(reconnect) && !widget.setSpectrumData(reconnect, {0.5f}) &&
            submit(replacement), "widget accepted output from a different source request");
}

} // namespace

// 不显示窗口。外层 CTest 超时覆盖线程无法退出的失败情形。
int main(int argc, char* argv[])
{
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "packet-epochs") {
            testPacketEpochs();
            return 0;
        }
        QApplication app(argc, argv);
        if (mode == "controller-local") testControllerLocal();
        else if (mode == "controller-supersede") testControllerSupersede();
        else if (mode == "widget-epochs") testWidgetEpochs();
        else throw std::runtime_error("expected controller-local, controller-supersede, packet-epochs or widget-epochs");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Session test failed: " << error.what() << '\n';
        return 1;
    }
}
