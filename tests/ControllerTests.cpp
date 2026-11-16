#include "MainWindow.h"
#include "PlayerController.h"
#include "RtspBlackhole.h"
#include "VideoWidget.h"

#include <QApplication>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>

// 通过主窗口自己的命令入口构造用例，并读取用于验证旧输出隔离的按值快照。
struct MainWindowCloseTestAccess {
    // 使用程序内部相同的打开入口，不引入面向用户的网络选项。
    static std::uint64_t open(MainWindow& window, const PlaybackSource& source)
    {
        return window.requestPlayback(source);
    }

    // 读取当前播放快照，便于验证关窗前后的输出归属。
    static PlaybackSnapshot snapshot(const MainWindow& window)
    {
        return window.player_->getPlaybackSnapshot();
    }

    // 走实际转码 worker 入口，测试不弹出选择文件的界面。
    static void startTranscode(MainWindow& window, const QString& input, const QString& output)
    {
        window.startTranscode(input, output);
    }
};

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

// 让已打开的纯视频 RTSP 在读包中欠缺剩余 TCP 数据，验证较早入队的包还能解码和提交。
void testControllerBlockedRead()
{
    VideoWidget widget;
    RtspBlackhole server(RtspBlackhole::StallAt::VideoReadPacket);
    PlayerController player;
    player.setVideoWidget(&widget);

    const auto source = PlaybackSource::networkLive(server.url(), {RtspTransport::Tcp, LivePreset::LowLatency});
    const auto request = player.requestOpen(source);
    require(request > 0, "controller did not accept the video request");
    waitUntil([&] { return server.isHoldingConnection(); }, 2s, "RTSP server did not send its partial record");
    const auto silence_started = Clock::now();

    PlaybackSnapshot queued;
    waitUntil([&] {
        queued = player.getPlaybackSnapshot();
        return queued.request_id == request && queued.state == PlaybackState::Playing &&
               queued.media_info.has_video && !queued.media_info.has_audio && queued.stats.video_queue_packets >= 30;
    }, 1s, "RTSP probe did not complete and queue the video burst");
    require(server.isHoldingConnection(), "RTSP closed after probing");
    std::cout << "during_stall_initial_video_queue=" << queued.stats.video_queue_packets
              << " initial_media_seconds=" << queued.current_seconds << std::endl;

    PlaybackSnapshot progressed;
    waitUntil([&] {
        progressed = player.getPlaybackSnapshot();
        return server.isHoldingConnection() && progressed.request_id == request &&
               progressed.state == PlaybackState::Playing && !progressed.last_error &&
               progressed.has_current_video_output && progressed.current_seconds >= queued.current_seconds + 0.5 &&
               progressed.stats.video_queue_packets + 6 <= queued.stats.video_queue_packets;
    }, 1600ms, "queued video did not decode and reach the widget while RTSP read remained blocked");
    const auto progressed_at = Clock::now();
    std::cout << "during_stall_video_queue=" << progressed.stats.video_queue_packets
              << " media_seconds=" << progressed.current_seconds
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(progressed_at - silence_started).count()
              << std::endl;

    // 停止在同一连接仍欠半包时提交，同时检查命令返回、会话退出、对端 socket 被关闭。
    const auto stop_at = Clock::now();
    require(player.stopAsync() == request && Clock::now() - stop_at < 500ms,
            "stop command blocked behind the RTSP read");
    waitUntil([&] {
        const auto snapshot = player.getPlaybackSnapshot();
        return snapshot.request_id == request && snapshot.state == PlaybackState::Idle &&
               !snapshot.is_playing && !snapshot.has_current_video_output &&
               !snapshot.last_error && !server.isHoldingConnection();
    }, 1200ms, "stopping did not cancel the RTSP read and close the session");
    const auto idle_at = Clock::now();
    require(idle_at - silence_started < playbackPolicy(source).read_timeout,
            "RTSP read was released only after the read deadline");
    std::cout << "blocked_read_stop_to_idle_and_disconnect_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(idle_at - stop_at).count() << '\n';
    player.shutdown();
}

// 离屏窗口也运行实际 closeEvent 和延迟删除，失败时回收仍存活的测试窗口。
struct TestWindow {
    QPointer<MainWindow> view = new MainWindow;

    // 窗口只有正常接受关闭后才由 Qt 删除。
    TestWindow() { view->setAttribute(Qt::WA_DeleteOnClose); }
    // 若断言失败导致窗口未关闭，仍走析构的线程收尾。
    ~TestWindow() { delete view.data(); }
};

// 等待窗口或夹具达到测试前置条件，同时让 Qt 线程持续分发事件。
template<typename Predicate>
void waitInQt(Predicate ready, Clock::duration timeout, const char* detail)
{
    if (ready()) return;
    QEventLoop loop;
    QTimer poll;
    const auto deadline = Clock::now() + timeout;
    bool satisfied = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        satisfied = ready();
        if (satisfied || Clock::now() >= deadline) loop.quit();
    });
    poll.start(5);
    loop.exec();
    require(satisfied, detail);
}

// CTest 默认只使用回环地址，显式环境变量可复测私有接口。
std::string testRtspAddress()
{
    return qEnvironmentVariable("MEDIASTUDIO_TEST_RTSP_ADDRESS", "127.0.0.1").toStdString();
}

// 检查已接到关窗命令的快照中不再有旧源标记或可显示内容。
bool outputRetired(const PlaybackSnapshot& snapshot, std::uint64_t request)
{
    return snapshot.request_id == request && snapshot.connection_attempt_id == 0 && snapshot.generation == 0 &&
           !snapshot.is_playing && !snapshot.has_current_video_output && !snapshot.last_error &&
           (snapshot.state == PlaybackState::Stopping || snapshot.state == PlaybackState::Idle);
}

// 在 Qt 事件回调里发出关窗，再验证事件分发、拒绝迟到请求和实际销毁。
Clock::time_point closeWindow(QPointer<MainWindow>& view, std::uint64_t request, const char* stage,
                              const std::function<void()>& before_close = {})
{
    QEventLoop loop;
    QTimer modal_watch;
    Clock::time_point requested;
    Clock::time_point returned;
    Clock::time_point destroyed;
    bool first_close_deferred = false;
    bool repeat_close_deferred = false;
    bool heartbeat_while_alive = false;
    bool late_open_rejected = false;
    bool old_output_retired = false;
    bool showed_modal = false;
    QObject::connect(&modal_watch, &QTimer::timeout, &loop, [&] {
        if (auto* modal = QApplication::activeModalWidget()) {
            showed_modal = true;
            // 防止意外的模态对话框吞掉事件循环退出，随后仍要报告测试失败。
            modal->close();
        }
    });
    modal_watch.start(5);
    QObject::connect(view.data(), &QObject::destroyed, &loop, [&] {
        destroyed = Clock::now();
        loop.quit();
    });
    QTimer::singleShot(2500ms, &loop, &QEventLoop::quit);
    QTimer::singleShot(0, &loop, [&] {
        if (before_close) before_close();
        requested = Clock::now();
        first_close_deferred = !view->close();
        returned = Clock::now();
        if (!view || !first_close_deferred) return;
        repeat_close_deferred = !view->close();
        QTimer::singleShot(0, &loop, [&] {
            heartbeat_while_alive = !view.isNull();
            if (!view) return;
            const auto late = PlaybackSource::localFile("not-opened-during-window-shutdown.avi");
            late_open_rejected = MainWindowCloseTestAccess::open(*view, late) == 0;
            old_output_retired = outputRetired(MainWindowCloseTestAccess::snapshot(*view), request);
        });
    });
    loop.exec();

    require(first_close_deferred && repeat_close_deferred, "close did not defer until playback shutdown completed");
    require(returned - requested < 100ms, "close command exceeded the 100 ms UI target");
    require(heartbeat_while_alive, "Qt could not dispatch a queued event while awaiting shutdown");
    require(late_open_rejected && old_output_retired, "closing window accepted a late request or retained old output");
    require(view.isNull() && destroyed - requested < 2s, "window did not finish playback shutdown and delete within 2 s");
    require(!showed_modal && !QApplication::activeModalWidget(), "late completion opened a modal dialog during shutdown");
    std::cout << "window_close_stage=" << stage
              << " command_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(returned - requested).count()
              << " destroy_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(destroyed - requested).count()
              << " heartbeat_while_alive=1 late_open_rejected=1 old_output_retired=1 modal=0\n";
    return requested;
}

// 同样的夹具在独立会话中让解码器按期限返回，精确确认挂起位置是探测流。
void verifyProbePhase(const std::string& address)
{
    RtspBlackhole server(RtspBlackhole::StallAt::ProbeStreams, address);
    const auto source = PlaybackSource::networkLive(server.url());
    MediaDecoder decoder;
    const auto started = Clock::now();
    const auto result = decoder.open(source);
    const auto elapsed = Clock::now() - started;
    require(result.status == MediaOpenStatus::Timeout && result.stage == MediaOpenStage::ProbeStreams,
            "fixture did not time out in avformat_find_stream_info");
    std::cout << "probe_precheck_stage=ProbeStreams ffmpeg_error=" << result.ffmpegError
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
}

// 在真实 RTSP 打开、探测或读包受阻时，关闭实际主窗口并观察对端收尾。
void testWindowCloseRtsp(RtspBlackhole::StallAt stage, const char* name)
{
    const auto address = testRtspAddress();
    if (stage == RtspBlackhole::StallAt::ProbeStreams) verifyProbePhase(address);
    RtspBlackhole server(stage, address);
    TestWindow window;
    const auto source = PlaybackSource::networkLive(server.url());
    const auto request = MainWindowCloseTestAccess::open(*window.view, source);
    require(request > 0, "window did not accept test RTSP source");
    waitInQt([&] { return server.isHoldingConnection(); }, 2s, "RTSP did not reach fixture stall point");
    const auto held_at = Clock::now();
    const bool read_stage = stage == RtspBlackhole::StallAt::VideoReadPacket;
    PlaybackSnapshot snapshot;
    waitInQt([&] {
        snapshot = MainWindowCloseTestAccess::snapshot(*window.view);
        if (!server.isHoldingConnection() || snapshot.request_id != request || snapshot.last_error) return false;
        if (read_stage) {
            // 60 帧小于读包高水位，已提交帧、未空队列和服务端半包共同证实经过了探测。
            return snapshot.state == PlaybackState::Playing && snapshot.has_current_video_output &&
                   snapshot.current_seconds >= 0.15 && snapshot.stats.video_queue_packets >= 15;
        }
        // PLAY 回应已发出，留出调度时间让 FFmpeg 进入已独立确认的探测阶段。
        return snapshot.state == PlaybackState::Opening && Clock::now() - held_at >= 100ms;
    }, 1500ms, "window did not reach the expected RTSP open, probe or read phase");
    std::cout << "window_rtsp_stall=" << name << " bind=" << address << " socket_held=1"
              << " video_queue=" << snapshot.stats.video_queue_packets
              << " media_seconds=" << snapshot.current_seconds << '\n';
    const auto requested = closeWindow(window.view, request, name);
    waitInQt([&] { return !server.isHoldingConnection(); }, 500ms, "RTSP peer did not observe connection close");
    const auto disconnected = Clock::now();
    require(disconnected - requested < 2s && disconnected - held_at < 5s,
            "RTSP connection was closed after cancellation target or deadline");
    std::cout << "window_rtsp_stage=" << name << " cancel_to_peer_closed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(disconnected - requested).count() << '\n';
}

// 窗口先停止尚未打开的连接，紧接着使用正常文件入口继续播放，最后在本地播放中关闭。
void testWindowStopReopen()
{
    const VideoFixtures fixtures;
    RtspBlackhole server(RtspBlackhole::StallAt::OpenInput, testRtspAddress());
    TestWindow window;
    const auto blocked = MainWindowCloseTestAccess::open(*window.view, PlaybackSource::networkLive(server.url()));
    require(blocked > 0, "window did not accept blocked RTSP request");
    waitInQt([&] { return server.isHoldingConnection(); }, 2s, "RTSP open did not block before stop");
    const auto stopped = Clock::now();
    require(QMetaObject::invokeMethod(window.view.data(), "stopPlayback", Qt::DirectConnection),
            "could not invoke window stop action");
    const auto stop_return = Clock::now();
    require(stop_return - stopped < 100ms && outputRetired(MainWindowCloseTestAccess::snapshot(*window.view), blocked),
            "window stop blocked or failed to retire the prior request");

    window.view->setInitialFiles({fixtures.second().u8string()});
    const auto reopen_return = Clock::now();
    require(reopen_return - stop_return < 100ms, "reopening local media blocked the UI");
    PlaybackSnapshot reopened;
    waitInQt([&] {
        reopened = MainWindowCloseTestAccess::snapshot(*window.view);
        return reopened.request_id > blocked && reopened.state == PlaybackState::Playing &&
               reopened.media_info.video_info.width == 80 && reopened.has_current_video_output &&
               reopened.current_seconds >= 0.15 && !reopened.last_error && !server.isHoldingConnection();
    }, 2s, "stop then reopen did not close the old socket and play latest local video");
    std::cout << "window_stop_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(stop_return - stopped).count()
              << " reopen_command_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(reopen_return - stop_return).count()
              << " stop_to_local_output_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopped).count()
              << " local_media_seconds=" << reopened.current_seconds << '\n';
    closeWindow(window.view, reopened.request_id, "local-playing");
}

// 验证真实转码输入受阻时可关窗，并覆盖启动命令后立即关窗的竞争窗口。
void testWindowCloseTranscode()
{
    const QTemporaryDir outputs("mediastudio-transcode-close-XXXXXX");
    require(outputs.isValid(), "could not allocate transcode fixture output directory");
    const auto address = testRtspAddress();
    {
        RtspBlackhole server(RtspBlackhole::StallAt::OpenInput, address);
        TestWindow window;
        const auto output = outputs.filePath("blocked.mp4");
        MainWindowCloseTestAccess::startTranscode(*window.view, QString::fromStdString(server.url()), output);
        waitInQt([&] { return server.isHoldingConnection(); }, 2s, "transcode input did not block in RTSP open");
        const auto requested = closeWindow(window.view, 0, "transcode-open");
        waitInQt([&] { return !server.isHoldingConnection(); }, 500ms, "transcode did not close the RTSP peer");
        require(Clock::now() - requested < 2s && !QFileInfo::exists(output),
                "blocked transcode outlived shutdown or produced an output file");
        std::cout << "transcode_input_socket_closed=1 bind=" << address
                  << " cancel_to_peer_closed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - requested).count() << '\n';
    }

    RtspBlackhole server(RtspBlackhole::StallAt::OpenInput, address);
    TestWindow window;
    const auto output = outputs.filePath("starting.mp4");
    closeWindow(window.view, 0, "transcode-starting", [&] {
        MainWindowCloseTestAccess::startTranscode(*window.view, QString::fromStdString(server.url()), output);
    });
    require(!QFileInfo::exists(output), "transcode started after the cancellation was posted");
}

// 在真实打开文件槽的嵌套事件循环里关闭主窗口，确认槽返回后才销毁窗口。
void testWindowCloseModal()
{
    TestWindow window;
    QEventLoop loop;
    QTimer poll;
    bool close_deferred = false;
    bool dialog_rejected = false;
    bool slot_returned = false;
    bool destroyed_after_slot = false;
    bool deadline_hit = false;
    Clock::duration close_elapsed{};
    QObject::connect(window.view.data(), &QObject::destroyed, &loop, [&] {
        destroyed_after_slot = slot_returned;
        loop.quit();
    });
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->parentWidget() != window.view.data()) return;
        poll.stop();
        QObject::connect(dialog, &QDialog::rejected, &loop, [&] { dialog_rejected = !window.view.isNull(); });
        const auto started = Clock::now();
        close_deferred = !window.view->close() && !window.view.isNull();
        close_elapsed = Clock::now() - started;
    });
    poll.start(5);
    QTimer::singleShot(2500ms, &loop, [&] {
        deadline_hit = true;
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
        loop.quit();
    });
    QTimer::singleShot(0, &loop, [&] {
        slot_returned = QMetaObject::invokeMethod(window.view.data(), "openFile", Qt::DirectConnection);
    });
    loop.exec();

    require(close_deferred && close_elapsed < 100ms, "close while the file dialog was active did not defer promptly");
    require(dialog_rejected && !deadline_hit, "window close did not reject the active file dialog");
    require(window.view.isNull() && destroyed_after_slot, "window was not destroyed after the modal slot unwound");
    std::cout << "window_close_stage=qt-file-dialog command_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(close_elapsed).count()
              << " first_close_deferred=1 dialog_rejected=1 slot_returned_before_destruction=1\n";
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

// 离屏运行实际 Qt 事件循环。外层 CTest 超时覆盖线程无法退出的失败情形。
int main(int argc, char* argv[])
{
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "packet-epochs") {
            testPacketEpochs();
            return 0;
        }
        if (mode == "window-close-modal") {
            // 离屏用例只运行 Qt 文件对话框，不依赖操作系统的原生模态事件循环。
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        }
        QApplication app(argc, argv);
        app.setQuitOnLastWindowClosed(false);
        if (mode == "controller-local") testControllerLocal();
        else if (mode == "controller-supersede") testControllerSupersede();
        else if (mode == "controller-blocked-read") testControllerBlockedRead();
        else if (mode == "window-close-open") testWindowCloseRtsp(RtspBlackhole::StallAt::OpenInput, "open");
        else if (mode == "window-close-probe") testWindowCloseRtsp(RtspBlackhole::StallAt::ProbeStreams, "probe");
        else if (mode == "window-close-read") testWindowCloseRtsp(RtspBlackhole::StallAt::VideoReadPacket, "read");
        else if (mode == "window-stop-reopen") testWindowStopReopen();
        else if (mode == "window-close-transcode") testWindowCloseTranscode();
        else if (mode == "window-close-modal") testWindowCloseModal();
        else if (mode == "widget-epochs") testWidgetEpochs();
        else throw std::runtime_error("unknown session test");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Session test failed: " << error.what() << '\n';
        return 1;
    }
}
