#include "MainWindow.h"
#include "PlayerController.h"
#include "RtspBlackhole.h"
#include "VideoWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>

#include <mmdeviceapi.h>

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

// 观察实际窗口的播放控件与媒体库请求路由，不模拟控制器或数据库接口。
struct MainWindowNetworkTestAccess {
    struct Controls {
        bool seek_enabled;
        bool speed_enabled;
        int speed_index;
        QString elapsed;
        QString duration;
        QString metadata;
        std::uint64_t library_request;
    };

    // 同步读取窗口实际渲染的数据与待登记的本地播放请求号。
    static Controls controls(const MainWindow& window)
    {
        return {window.slider_progress_->isEnabled(), window.combo_speed_->isEnabled(), window.combo_speed_->currentIndex(),
                window.label_time_->text(), window.label_duration_->text(), window.text_media_info_->toPlainText(),
                window.library_request_id_};
    }

    // 检查网络地址只留在内存源，不进入 Qt 列表可见或附带字段。
    static bool hasSafeLiveRow(const MainWindow& window, const std::string& raw)
    {
        if (window.playlist_.size() != 3 || window.list_playlist_->count() != 3 || window.list_playlist_->currentRow() != 2 ||
            window.playlist_[0].kind() != PlaybackSourceKind::LocalFile ||
            window.playlist_[1].kind() != PlaybackSourceKind::LocalFile ||
            window.playlist_[2].kind() != PlaybackSourceKind::NetworkLive || window.playlist_[2].address() != raw) return false;
        const auto* item = window.list_playlist_->item(2);
        return item->text().contains("RTSP/UDP") && !item->text().contains("MediaStudioSecret") &&
               item->toolTip().isEmpty() && !item->data(Qt::UserRole).isValid();
    }

    // 从文件倍速控件实际触发速度选择。
    static void chooseSpeed(MainWindow& window, int index) { window.combo_speed_->setCurrentIndex(index); }

    // 直调界面和后端双重定位/倍速入口，验证禁用判断不依赖控件灰显。
    static void attemptLiveMutations(MainWindow& window)
    {
        window.seekPosition(999);
        window.seekRelative(500.0);
        window.changeSpeed(7);
        window.player_->seek(500.0);
        window.player_->setSpeed(4.0f);
    }

    // 读取控制器保存的本地首选速度，和当前源的播放能力独立验证。
    static float preferredSpeed(const MainWindow& window) { return window.player_->getSpeed(); }

    // 复用列表生产入口，避免测试另行维护“当前直播源”。
    static void openListed(MainWindow& window, const PlaybackSource& source)
    {
        window.openPlaylistEntry(window.appendPlaylistEntry(source));
    }
    // 删除列表项后活动会话仍可暂停/重新接入，不应读错变动后的行号。
    static void removeSelected(MainWindow& window) { window.removeFromPlaylist(); }
};

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// 定义在窗口事件辅助函数旁；本地和直播用例共用持续状态检查。
template<typename Predicate>
void remainsInQt(Predicate condition, Clock::duration duration, const char* detail);

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

    // 本地暂停只冻结原会话和位置，继续后不能从队列末端或文件开头跳播。
    require(player.pause() == second, "local pause changed request identity");
    const auto local_paused = player.getPlaybackSnapshot();
    remainsInQt([&] {
        const auto frozen = player.getPlaybackSnapshot();
        return frozen.state == PlaybackState::Paused && frozen.request_id == second &&
               frozen.connection_attempt_id == local_paused.connection_attempt_id &&
               std::abs(frozen.current_seconds - local_paused.current_seconds) <= 0.08;
    }, 150ms, "local media advanced or reopened while paused");
    require(player.play() == second, "local resume replaced the existing request");
    waitUntil([&] { return player.getPlaybackSnapshot().current_seconds >= local_paused.current_seconds + 0.12; },
              600ms, "local resume did not advance from the paused cursor");
    require(player.getPlaybackSnapshot().current_seconds <= local_paused.current_seconds + 0.45,
            "local resume jumped ahead instead of retaining the paused cursor");

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
        return queued.request_id == request &&
               (queued.state == PlaybackState::Buffering || queued.state == PlaybackState::Playing) &&
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

// 只判断 Windows 测试机是否有默认播放端点；设备存在时任何 PCM 提交失败都应使测试失败。
bool hasDefaultRenderEndpoint()
{
    HRESULT result = E_FAIL;
    std::thread worker([&] {
        result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result)) return;
        IMMDeviceEnumerator* enumerator = nullptr;
        result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(result)) {
            IMMDevice* endpoint = nullptr;
            result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
            if (endpoint) endpoint->Release();
            enumerator->Release();
        }
        CoUninitialize();
    });
    worker.join();
    if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return false;
    require(SUCCEEDED(result), "could not enumerate the default Windows playback endpoint");
    return true;
}

// 在观察窗口内持续分发 Qt 事件，不因短时状态碰巧满足而漏掉后台复活。
template<typename Predicate>
void remainsInQt(Predicate condition, Clock::duration duration, const char* detail)
{
    bool violated = !condition();
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!condition()) { violated = true; loop.quit(); }
    });
    poll.start(5);
    QTimer::singleShot(std::chrono::duration_cast<std::chrono::milliseconds>(duration), &loop, &QEventLoop::quit);
    loop.exec();
    require(!violated, detail);
}

// 标签取自实际快照，测试用它向控件模拟已退出 worker 的迟到帧和频谱。
PlaybackEpoch snapshotEpoch(const PlaybackSnapshot& snapshot)
{
    return {snapshot.request_id, snapshot.connection_attempt_id, snapshot.generation};
}

// 暂停/重接的无新输出快照不能接纳旧 epoch，也不能把留存画面算成新绘制。
void requireStaleOutputRejected(VideoWidget& widget, PlaybackEpoch old_epoch, PlaybackEpoch current_epoch)
{
    const std::array<std::uint8_t, 16> pixels{};
    const std::uint8_t* planes[3]{pixels.data(), nullptr, nullptr};
    const int strides[3]{8, 0, 0};
    require(!widget.setVideoFrame(old_epoch, VideoFrameFormat::RGBA, planes, strides, true, false, 2, 2) &&
            !widget.setSpectrumData(old_epoch, {0.8f}) && !widget.firstDrawnAt(current_epoch),
            "obsolete worker output or retained frame was accepted as current output");
}

// 暂停必须等实际 RTSP 对端关闭和 supervisor 完成收尾，而不只改变界面状态。
PlaybackSnapshot pauseLiveAndSettle(PlayerController& player, RtspBlackhole& server, unsigned int closed)
{
    const auto before = player.getPlaybackSnapshot();
    const auto started = Clock::now();
    require(player.pause() == before.request_id && Clock::now() - started < 100ms,
            "live pause blocked or changed the active request identity");
    const auto pending = player.getPlaybackSnapshot();
    require(pending.state == PlaybackState::Paused && pending.is_paused && !pending.is_playing &&
            pending.connection_attempt_id == 0 && pending.generation == 0 && !pending.has_current_video_output,
            "live pause did not immediately retire the previous output epoch");
    PlaybackSnapshot settled;
    waitInQt([&] {
        settled = player.getPlaybackSnapshot();
        return settled.state == PlaybackState::Paused && !settled.is_session_active &&
               server.closedConnections() == closed && !server.isHoldingConnection();
    }, 2s, "live pause did not cancel blocked IO and retire the actual RTSP session");
    require(!settled.last_error && settled.stats.audio_buffer_seconds == 0.0 &&
            settled.stats.audio_queue_packets == 0 && settled.stats.video_queue_packets == 0,
            "settled live pause left queued media, sound-card buffering, or a cancellation error");
    const auto accepted = server.acceptedConnections();
    remainsInQt([&] {
        const auto now = player.getPlaybackSnapshot();
        return now.state == PlaybackState::Paused && !now.is_session_active && !now.has_current_video_output &&
            now.current_seconds == settled.current_seconds && now.live_elapsed_seconds == settled.live_elapsed_seconds &&
            server.acceptedConnections() == accepted;
    }, 180ms, "paused media clock, output, or connection continued after session teardown");
    std::cout << "live_pause_disconnect_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()
              << " closed=" << closed << " no_queued_audio_or_video=1\n";
    return settled;
}

// Opening/Probe/阻塞读各暂停一次；继续必须重新打开同一 URL，回到直播独立替换当前请求。
void testLivePauseStage(std::string_view mode)
{
    using Fixture = RtspBlackhole;
    using Stall = Fixture::StallAt;
    const auto stage = mode == "live-pause-open" ? Stall::OpenInput
        : mode == "live-pause-probe" ? Stall::ProbeStreams : Stall::VideoReadPacket;
    Fixture server({stage, Stall::VideoReadPacket, Stall::OpenInput}, testRtspAddress(), Fixture::SetupTransport::Tcp);
    VideoWidget widget;
    PlayerController player;
    player.setVideoWidget(&widget);
    const auto source = createStableRtspSource(server.url(), RtspTransport::Tcp);
    const auto first = player.requestOpen(*source.source);
    waitInQt([&] { return server.isHoldingConnection(); }, 2s, "first RTSP session did not reach selected blocking stage");
    if (stage == Stall::VideoReadPacket) {
        waitInQt([&] { return player.getPlaybackSnapshot().has_current_video_output; }, 1s,
                 "video read fixture never produced a frame before pause");
    } else {
        require(player.getPlaybackSnapshot().state == PlaybackState::Opening &&
                (stage != Stall::ProbeStreams || server.playRequests() == 1),
                "pause did not target an actual incomplete open/probe operation");
    }
    const auto old_epoch = snapshotEpoch(player.getPlaybackSnapshot());
    const auto paused = pauseLiveAndSettle(player, server, 1);
    requireStaleOutputRejected(widget, old_epoch, snapshotEpoch(paused));

    const auto resumed = player.play();
    require(resumed > first, "live resume reused its cancelled request");
    PlaybackSnapshot active;
    waitInQt([&] {
        active = player.getPlaybackSnapshot();
        return active.request_id == resumed && active.state == PlaybackState::Playing && active.has_current_video_output;
    }, 2s, "continue did not reconnect the same live source and produce fresh video");
    require(server.acceptedConnections() == 2 && active.rtsp_transport == RtspTransport::Tcp &&
            active.network_stats && active.network_stats->first_video_submit_seconds &&
            active.live_elapsed_seconds && *active.live_elapsed_seconds < 1.0,
            "resumed live request reused stale output metrics or changed transport");

    const auto reconnect = player.returnToLive();
    require(reconnect > resumed, "return to live did not replace the active request");
    waitInQt([&] { return server.acceptedConnections() == 3 && server.isHoldingConnection(); }, 2s,
             "return to live did not reconnect its existing source");
    const auto opening = player.getPlaybackSnapshot();
    require(opening.request_id == reconnect && opening.state == PlaybackState::Opening &&
            !opening.has_current_video_output && !opening.live_elapsed_seconds && opening.network_stats &&
            !opening.network_stats->first_packet_seconds && !opening.network_stats->first_pcm_seconds &&
            !opening.network_stats->first_video_submit_seconds && !opening.network_stats->first_video_draw_seconds,
            "retained frame or previous milestones counted as a reopened connection output");
    requireStaleOutputRejected(widget, snapshotEpoch(active), snapshotEpoch(opening));
    player.stopAsync();
    waitInQt([&] {
        const auto stopped = player.getPlaybackSnapshot();
        return stopped.state == PlaybackState::Idle && !stopped.is_session_active && server.closedConnections() == 3;
    }, 2s, "stop after continue did not cancel the final RTSP open");
    require(player.play() == 0 && player.returnToLive() == 0, "stop retained a source capable of resurrecting live playback");
    std::cout << "pause_stage=" << mode << " resume_new_request=1 explicit_reconnect_reset_metrics=1\n";
}

// 六种轨道/传输组合共用完整生命周期，避免只验证混合音画而漏掉单轨就绪条件。
int testLiveLifecycle(std::string_view mode)
{
    using Fixture = RtspBlackhole;
    const bool udp = mode.find("-udp") != std::string_view::npos;
    const auto media = mode.find("-video-") != std::string_view::npos ? Fixture::StartupMedia::Video
        : mode.find("-audio-") != std::string_view::npos ? Fixture::StartupMedia::Audio : Fixture::StartupMedia::AudioVideo;
    const bool audio = media != Fixture::StartupMedia::Video;
    const bool video = media != Fixture::StartupMedia::Audio;
    if (audio && !hasDefaultRenderEndpoint()) return 77;
    const auto transport = udp ? RtspTransport::Udp : RtspTransport::Tcp;
    Fixture server(media, testRtspAddress(),
        udp ? Fixture::SetupTransport::Udp : Fixture::SetupTransport::Tcp, Fixture::KeyVideo::Immediate, 8, 3);
    VideoWidget widget;
    PlayerController player;
    player.setVideoWidget(&widget);
    player.setVolume(0.27f);
    player.setEffect(VideoEffect::Grayscale);
    player.toggleVisualizer();
    const auto source = createStableRtspSource(server.url(), transport);
    auto request = player.requestOpen(*source.source);
    for (unsigned int round = 1; round <= 3; ++round) {
        PlaybackSnapshot active;
        waitInQt([&] {
            active = player.getPlaybackSnapshot();
            return active.request_id == request && active.state == PlaybackState::Playing &&
                   active.has_current_video_output == video && active.network_stats &&
                   active.network_stats->first_pcm_seconds.has_value() == audio &&
                   active.network_stats->first_video_submit_seconds.has_value() == video && active.live_elapsed_seconds;
        }, 2s, "live connection did not submit the expected PCM/video tracks");
        require(active.rtsp_transport == transport && std::abs(player.getVolume() - 0.27f) < 0.001f &&
                player.getEffect() == VideoEffect::Grayscale && player.isVisualizerShown() &&
                *active.live_elapsed_seconds < 1.0 && !active.network_stats->first_video_draw_seconds,
                "live reconnect lost volume/effect/visualizer/transport or reused the previous connection clock");
        const auto epoch = snapshotEpoch(active);
        if (round == 1) {
            const auto paused = pauseLiveAndSettle(player, server, round);
            require(server.teardownRequests() == 1 && server.activeRemoteSessions() == 0,
                    "pause closed its control socket without TEARDOWN of the remote RTP session");
            requireStaleOutputRejected(widget, epoch, snapshotEpoch(paused));
        } else if (round == 3) {
            require(player.stopAsync() == request, "lifecycle stop did not target the active live session");
            waitInQt([&] {
                const auto stopped = player.getPlaybackSnapshot();
                return stopped.state == PlaybackState::Idle && !stopped.is_session_active &&
                       server.closedConnections() == 3 && server.teardownRequests() == 3 && server.activeRemoteSessions() == 0;
            }, 2s, "lifecycle stop did not release all local and remote sessions");
            requireStaleOutputRejected(widget, epoch, {});
            break;
        }
        const auto reopened = round == 1 ? player.togglePlayPause() : player.returnToLive();
        require(reopened > request, "AV resume/return-to-live did not allocate a new request");
        request = reopened;
        const auto opening = player.getPlaybackSnapshot();
        require(opening.request_id == request && opening.network_stats &&
                (!opening.live_elapsed_seconds || *opening.live_elapsed_seconds < 1.0),
                "continued or explicit live replacement inherited the previous connection clock");
        if (opening.state == PlaybackState::Opening) {
            require(!opening.has_current_video_output && !opening.live_elapsed_seconds &&
                    !opening.network_stats->first_pcm_seconds && !opening.network_stats->first_video_submit_seconds,
                    "reopened connection counted the retained frame as fresh output before probing");
        }
        requireStaleOutputRejected(widget, epoch, snapshotEpoch(opening));
    }
    require(server.acceptedConnections() == 3 && server.playRequests() == 3,
            "AV pause/resume created duplicate connections or failed to issue a fresh RTSP PLAY");
    std::cout << "live_lifecycle=" << mode
              << " output_rounds=3 pause_resume_return_stop=1 preserved_preferences=1 teardown_requests=" << server.teardownRequests()
              << " active_remote_sessions=" << server.activeRemoteSessions() << '\n';
    return 0;
}

// 同一控制器完成 20 次实际文件/直播切换，每次都等新会话输出，而非只投递打开命令。
int testMixedSwitchStress(bool udp)
{
    if (!hasDefaultRenderEndpoint()) return 77;
    using Fixture = RtspBlackhole;
    Fixture server(Fixture::StartupMedia::AudioVideo, testRtspAddress(),
        udp ? Fixture::SetupTransport::Udp : Fixture::SetupTransport::Tcp, Fixture::KeyVideo::Immediate, 8, 10);
    const VideoFixtures files;
    VideoWidget widget;
    PlayerController player;
    player.setVideoWidget(&widget);
    player.setVolume(0.23f);
    player.setEffect(VideoEffect::Sepia);
    player.setSpeed(1.5f);
    player.toggleVisualizer();
    const auto live = createStableRtspSource(server.url(), udp ? RtspTransport::Udp : RtspTransport::Tcp);
    std::vector<PlaybackEpoch> retired;
    unsigned int network_sessions = 0;
    std::uint64_t prior_request = 0;
    for (unsigned int step = 0; step <= 20; ++step) {
        const bool network = step % 2 == 1;
        const auto source = network ? *live.source : PlaybackSource::localFile(files.second().u8string());
        const auto request = player.requestOpen(source);
        require(request > prior_request, "mixed switch reused a retired playback request");
        PlaybackSnapshot output;
        waitInQt([&] {
            output = player.getPlaybackSnapshot();
            return output.request_id == request && output.state == PlaybackState::Playing && output.has_current_video_output &&
                (network ? output.network_stats && output.network_stats->first_pcm_seconds
                         : output.media_info.video_info.width == 80 && output.current_seconds >= 0.12);
        }, 2s, "mixed switch did not deliver real output for the newest source");
        const auto current = snapshotEpoch(output);
        require(current.connection_attempt_id != 0 && current.generation != 0 && !output.last_error &&
                std::abs(player.getVolume() - 0.23f) < 0.001f && player.getEffect() == VideoEffect::Sepia &&
                player.isVisualizerShown() && std::abs(player.getSpeed() - 1.5f) < 0.001f,
                "mixed source output lost its epoch or user preferences");
        for (const auto old : retired) requireStaleOutputRejected(widget, old, current);
        if (network) {
            ++network_sessions;
            require(output.source_kind == PlaybackSourceKind::NetworkLive && output.media_info.has_audio &&
                    output.current_seconds >= 2699.0 && output.rtsp_transport == live.source->liveOptions()->rtsp_transport &&
                    output.live_elapsed_seconds && *output.live_elapsed_seconds < 1.0 &&
                    server.acceptedConnections() == network_sessions && server.activeRemoteSessions() == 1,
                    "new live connection inherited a local clock or changed transport/session ownership");
        } else {
            waitInQt([&] { return server.closedConnections() == network_sessions &&
                server.teardownRequests() == network_sessions && server.activeRemoteSessions() == 0; },
                500ms, "switching to a file left the old RTSP session allocated");
            // 本地文件无音轨；旧声卡的约 2700 秒媒体时钟不能回写到新文件。
            remainsInQt([&] {
                const auto local = player.getPlaybackSnapshot();
                return local.request_id == request && local.source_kind == PlaybackSourceKind::LocalFile &&
                    !local.media_info.has_audio && local.current_seconds < 1.0 && local.stats.audio_queue_packets == 0 &&
                    local.stats.audio_buffer_seconds == 0.0 && !local.network_stats;
            }, 80ms, "retired live audio or network metrics flowed back into the new local file");
        }
        retired.push_back(current);
        prior_request = request;
        std::cout << "mixed_output_step=" << step << " request=" << request << " source="
                  << (network ? "RTSP" : "file") << " media_seconds=" << output.current_seconds << '\n';
    }
    player.stopAsync();
    waitInQt([&] { return player.getPlaybackSnapshot().state == PlaybackState::Idle &&
        !player.getPlaybackSnapshot().is_session_active; }, 1s, "stress controller did not stop");
    std::cout << "mixed_switch_transport=" << (udp ? "UDP" : "TCP")
              << " actual_outputs=21 switches=20 retired_epochs_checked=210 remote_sessions_released="
              << server.teardownRequests() << '\n';
    return 0;
}

// 连接失败及已输出后的真实读取超时都必须是明确终态，已退出 worker 不能再提交输出。
void testLiveFailures(bool udp)
{
    using Fixture = RtspBlackhole;
    const auto transport = udp ? RtspTransport::Udp : RtspTransport::Tcp;
    VideoWidget widget;
    PlayerController player;
    player.setVideoWidget(&widget);
    Fixture refusing(Fixture::StallAt::RejectOpen, testRtspAddress());
    const auto rejected_source = createStableRtspSource(refusing.url(), transport);
    const auto rejected_id = player.requestOpen(*rejected_source.source);
    PlaybackSnapshot refused;
    waitInQt([&] {
        refused = player.getPlaybackSnapshot();
        return refused.request_id == rejected_id && refused.state == PlaybackState::Failed && !refused.is_session_active;
    }, 4s, "connection refusal did not become an explicit Failed state");
    const auto* open_error = refused.last_error ? std::get_if<MediaOpenResult>(&*refused.last_error) : nullptr;
    require(open_error && open_error->status == MediaOpenStatus::Error && open_error->stage == MediaOpenStage::OpenInput &&
            !refused.is_playing && !refused.has_current_video_output &&
            open_error->ffmpegError == AVERROR_HTTP_FORBIDDEN && refusing.acceptedConnections() == 1,
            "RTSP 403 rejection was reported as playback or lost its raw open error");

    // 空闲回环端口在不同系统可立刻拒绝或等 SYN 超时；两者都应发布有界的打开失败。
    Fixture absent(Fixture::StallAt::UnavailableEndpoint, testRtspAddress());
    const auto absent_source = createStableRtspSource(absent.url(), transport);
    const auto absent_id = player.requestOpen(*absent_source.source);
    PlaybackSnapshot unavailable;
    waitInQt([&] {
        unavailable = player.getPlaybackSnapshot();
        return unavailable.request_id == absent_id && unavailable.state == PlaybackState::Failed && !unavailable.is_session_active;
    }, 7s, "unreachable RTSP endpoint did not report failure within the configured open deadline");
    const auto* absent_error = unavailable.last_error ? std::get_if<MediaOpenResult>(&*unavailable.last_error) : nullptr;
    require(absent_error && absent_error->stage == MediaOpenStage::OpenInput && absent_error->ffmpegError < 0 &&
            (absent_error->status == MediaOpenStatus::Error || absent_error->status == MediaOpenStatus::Timeout) &&
            !unavailable.is_playing && !unavailable.has_current_video_output && absent.acceptedConnections() == 0,
            "unreachable endpoint lost its open error or was confused with usable playback");

    Fixture silent({Fixture::StallAt::VideoReadPacket, Fixture::StallAt::VideoReadPacket}, testRtspAddress(),
        udp ? Fixture::SetupTransport::Udp : Fixture::SetupTransport::Tcp);
    const auto source = createStableRtspSource(silent.url(), transport);
    const auto request = player.requestOpen(*source.source);
    PlaybackSnapshot playing;
    waitInQt([&] {
        playing = player.getPlaybackSnapshot();
        return playing.request_id == request && playing.state == PlaybackState::Playing && playing.has_current_video_output;
    }, 2s, "read-timeout fixture failed before producing actual video output");
    const auto previous = snapshotEpoch(playing);
    PlaybackSnapshot failed;
    waitInQt([&] {
        failed = player.getPlaybackSnapshot();
        return failed.request_id == request && failed.state == PlaybackState::Failed && !failed.is_session_active;
    }, 7s, "silent RTSP transport remained Playing past the read deadline");
    const auto* read_error = failed.last_error ? std::get_if<MediaReadResult>(&*failed.last_error) : nullptr;
    waitInQt([&] { return silent.closedConnections() == 1 && silent.activeRemoteSessions() == 0; },
             500ms, "read failure did not release its remote RTSP session");
    std::cout << "read_failure_status=" << (read_error ? static_cast<int>(read_error->status) : -1)
              << " stage=" << (read_error ? static_cast<int>(read_error->stage) : -1)
              << " ffmpeg_raw=" << (read_error ? read_error->ffmpegError : 0)
              << " epoch=" << failed.connection_attempt_id << '/' << failed.generation
              << " current_video=" << failed.has_current_video_output << " playing=" << failed.is_playing
              << " paused=" << failed.is_paused << " audio_buffer=" << failed.stats.audio_buffer_seconds
              << " closed=" << silent.closedConnections() << " teardown=" << silent.teardownRequests() << '\n';
    require(read_error && read_error->status == MediaReadStatus::Timeout && read_error->stage == MediaReadStage::ReadPacket &&
            // FFmpeg 可在期限刚过时返回最后一个缓冲包（0）；原始码与 Timeout 分类独立保存。
            read_error->ffmpegError <= 0 && !failed.is_playing && !failed.is_paused && !failed.has_current_video_output &&
            failed.connection_attempt_id == 0 && failed.generation == 0 &&
            failed.stats.audio_buffer_seconds == 0.0 && silent.closedConnections() == 1 && silent.activeRemoteSessions() == 0,
            "read timeout did not retire live output and release its remote session");
    requireStaleOutputRejected(widget, previous, snapshotEpoch(failed));
    remainsInQt([&] {
        const auto now = player.getPlaybackSnapshot();
        return now.request_id == request && now.state == PlaybackState::Failed && !now.is_session_active &&
               now.current_seconds == failed.current_seconds && now.live_elapsed_seconds == failed.live_elapsed_seconds &&
               silent.acceptedConnections() == 1;
    }, 180ms, "failed live request resumed media time or automatically reconnected");
    const auto reopened = player.returnToLive();
    require(reopened > request, "failed request could not be explicitly replaced by a fresh live connection");
    PlaybackSnapshot recovered;
    waitInQt([&] {
        recovered = player.getPlaybackSnapshot();
        return recovered.request_id == reopened && recovered.state == PlaybackState::Playing &&
               recovered.has_current_video_output && recovered.live_elapsed_seconds && !recovered.last_error;
    }, 2s, "explicit return-to-live did not resume output after a read failure");
    require(snapshotEpoch(recovered) != previous && *recovered.live_elapsed_seconds < 1.0 &&
            silent.acceptedConnections() == 2, "manual recovery reused the failed output epoch or playback clock");
    requireStaleOutputRejected(widget, previous, snapshotEpoch(recovered));
    player.stopAsync();
    waitInQt([&] { return silent.closedConnections() == 2 && silent.activeRemoteSessions() == 0 &&
        player.getPlaybackSnapshot().state == PlaybackState::Idle; }, 1s, "stop after manual recovery left a live session");
    std::cout << "live_failure_transport=" << (udp ? "UDP" : "TCP")
              << " refused_open_error=" << open_error->ffmpegError
              << " unavailable_open_status=" << static_cast<int>(absent_error->status)
              << " after_output_read_timeout=1 retired_output=1 explicit_return_after_failed=1\n";
}

// 快速手动重接/继续被停止、切源和关机覆盖后，旧连接不得再次复活。
void testLiveCommandRaces()
{
    using Fixture = RtspBlackhole;
    const VideoFixtures files;
    for (const int terminal : {0, 1, 2}) {
        Fixture server(std::vector<Fixture::StallAt>(24, Fixture::StallAt::OpenInput),
                       testRtspAddress(), Fixture::SetupTransport::Tcp);
        VideoWidget widget;
        PlayerController player;
        player.setVideoWidget(&widget);
        const auto source = createStableRtspSource(server.url(), RtspTransport::Tcp);
        player.requestOpen(*source.source);
        waitInQt([&] { return server.isHoldingConnection(); }, 1s, "rapid-command fixture did not accept its initial connection");
        for (int i = 0; i < 8; ++i) {
            require(player.returnToLive() > 0 && player.pause() > 0 && player.play() > 0,
                    "live command burst rejected a valid retained source");
        }
        std::uint64_t latest = player.getPlaybackSnapshot().request_id;
        if (terminal == 0) {
            player.stopAsync();
            require(player.play() == 0 && player.returnToLive() == 0, "stopped command burst retained an active source");
        } else if (terminal == 1) {
            latest = player.requestOpen(PlaybackSource::localFile(files.second().u8string()));
        } else {
            player.requestShutdown();
            require(player.play() == 0 && player.returnToLive() == 0 && player.pause() == 0 &&
                    player.requestOpen(*source.source) == 0, "shutdown admitted new playback commands");
        }
        waitInQt([&] {
            const auto settled = player.getPlaybackSnapshot();
            if (terminal == 1) return settled.request_id == latest && settled.source_kind == PlaybackSourceKind::LocalFile &&
                settled.has_current_video_output && settled.media_info.video_info.width == 80 && !server.isHoldingConnection();
            return settled.request_id == latest && settled.state == PlaybackState::Idle && !settled.is_session_active &&
                !server.isHoldingConnection() && (terminal != 2 || player.isShutdownComplete());
        }, 2s, "newest stop/local/shutdown command did not supersede rapid live requests");
        const auto accepted = server.acceptedConnections();
        remainsInQt([&] {
            const auto current = player.getPlaybackSnapshot();
            return current.request_id == latest && !current.last_error && server.acceptedConnections() == accepted &&
                !server.isHoldingConnection() && (terminal == 1 ? current.source_kind == PlaybackSourceKind::LocalFile
                                                              : current.state == PlaybackState::Idle);
        }, 250ms, "cancelled pending live command reopened after its terminal replacement");
        std::cout << "rapid_live_terminal=" << terminal << " accepted=" << accepted << " no_late_reopen=1\n";
    }
}

// 用真实播放器从首包走到输出；只收到非关键包时不能离开 Buffering。
int testLiveStartup(std::string_view mode)
{
    using Fixture = RtspBlackhole;
    const bool native = mode == "live-start-native";
    const bool no_key = mode == "live-start-no-key";
    const bool offset_start = mode == "live-start-av-offset-tcp";
    const auto media = mode.find("-av-") != std::string_view::npos ? Fixture::StartupMedia::AudioVideo
        : mode.find("-audio-") != std::string_view::npos ? Fixture::StartupMedia::Audio : Fixture::StartupMedia::Video;
    const bool audio = media != Fixture::StartupMedia::Video;
    const bool video = media != Fixture::StartupMedia::Audio;
    const bool udp = mode.find("-udp") != std::string_view::npos;
    if (audio && !hasDefaultRenderEndpoint()) {
        std::cout << "Skipped: no default Windows audio endpoint for real WASAPI PCM submission\n";
        return 77;
    }

    VideoWidget widget;
    widget.setShowSpectrum(audio || native);
    if (native) {
        require(QApplication::platformName() == "windows", "native startup test requires the Windows Qt platform");
        widget.resize(360, 260);
        widget.show();
    }
    const auto expected_transport = udp ? Fixture::SetupTransport::Udp : Fixture::SetupTransport::Tcp;
    const auto key_video = no_key ? Fixture::KeyVideo::NeverSend
        : video && !offset_start ? Fixture::KeyVideo::AwaitRelease : Fixture::KeyVideo::Immediate;
    Fixture server(media, testRtspAddress(), expected_transport, key_video, offset_start ? 8 : 0);
    PlayerController player;
    player.setVideoWidget(&widget);
    const auto source = createStableRtspSource(server.url(), udp ? RtspTransport::Udp : RtspTransport::Tcp);
    require(source.source.has_value(), "fixture playback source failed validation");
    const auto started = Clock::now();
    const auto request = player.requestOpen(*source.source);
    require(request > 0, "live supervisor rejected the fixture request");

    if (video && !offset_start) {
        PlaybackSnapshot waiting;
        waitInQt([&] {
            waiting = player.getPlaybackSnapshot();
            return waiting.request_id == request && waiting.network_stats && waiting.network_stats->first_packet_seconds &&
                   (!audio || waiting.network_stats->first_pcm_seconds) &&
                   (waiting.state == PlaybackState::Buffering || waiting.state == PlaybackState::Playing);
        }, 3s, "RTSP did not deliver packet/PCM milestones while waiting for a key packet");
        require(server.isHoldingConnection() && waiting.state == PlaybackState::Buffering &&
                waiting.media_info.has_video && waiting.media_info.has_audio == audio &&
                !waiting.has_current_video_output && !waiting.network_stats->first_video_submit_seconds &&
                !waiting.network_stats->first_video_draw_seconds,
                "packet receipt or audio-only output prematurely marked all live tracks ready");
        std::cout << "buffering_before_idr=1 pcm_ready_before_idr=" << audio << '\n';
        if (no_key) {
            // 服务器持续供包，只是没有关键帧；首输出期限独立于读取超时。
            const auto buffered_at = Clock::now();
            PlaybackSnapshot terminal;
            waitInQt([&] {
                terminal = player.getPlaybackSnapshot();
                return terminal.request_id == request && terminal.state == PlaybackState::Failed;
            }, 12s, "nonkey packets kept an unusable live session alive without bound");
            const auto elapsed = Clock::now() - buffered_at;
            const auto* error = terminal.last_error ? std::get_if<MediaReadResult>(&*terminal.last_error) : nullptr;
            require(error && error->status == MediaReadStatus::Timeout && error->stage == MediaReadStage::InitialOutput &&
                    elapsed >= 9s && !terminal.has_current_video_output && terminal.network_stats &&
                    terminal.network_stats->first_packet_seconds && !terminal.network_stats->first_video_submit_seconds,
                    "nonkey stream failed outside the independent initial output deadline");
            std::cout << "startup_no_key_initial_output_timeout_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
            player.shutdown();
            return 0;
        }
        server.releaseKeyVideo();
    }

    PlaybackSnapshot playing;
    waitInQt([&] {
        playing = player.getPlaybackSnapshot();
        return playing.request_id == request && (playing.state == PlaybackState::Failed ||
            (playing.state == PlaybackState::Playing && playing.current_seconds >= 2699.0 && playing.live_elapsed_seconds));
    }, audio && video ? 8s : 4s, "RTSP did not deliver output promptly at the large source PTS");
    if (playing.state == PlaybackState::Failed) {
        const auto* error = playing.last_error ? std::get_if<MediaReadResult>(&*playing.last_error) : nullptr;
        std::cerr << "startup_failure_elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()
                  << " media_pts=" << playing.current_seconds << " ffmpeg=" << (error ? error->ffmpegError : 0) << '\n';
    }
    require(server.setupTransport() == expected_transport && playing.media_info.has_audio == audio &&
            playing.state == PlaybackState::Playing && playing.media_info.has_video == video &&
            playing.has_current_video_output == video &&
            playing.network_stats.has_value() && !playing.last_error, "opened live tracks, transport, or output did not match");
    const auto& milestones = *playing.network_stats;
    require(milestones.first_packet_seconds.has_value() && *milestones.first_packet_seconds >= 0.0 &&
            milestones.first_pcm_seconds.has_value() == audio && milestones.first_video_submit_seconds.has_value() == video &&
            (!audio || *milestones.first_pcm_seconds >= *milestones.first_packet_seconds) &&
            (!video || *milestones.first_video_submit_seconds >= *milestones.first_packet_seconds) &&
            (!(audio && video) || *milestones.first_pcm_seconds <= *milestones.first_video_submit_seconds) &&
            !milestones.first_video_draw_seconds && *playing.live_elapsed_seconds < 5.0,
            "output milestones did not distinguish actual PCM/frame submission, draw, and playback time");

    const PlaybackEpoch epoch{playing.request_id, playing.connection_attempt_id, playing.generation};
    require(!widget.firstDrawnAt(epoch), "hidden/spectrum view falsely reported a video draw");
    if (offset_start) {
        // 无有效 PTS 的首帧可先提交；统计窗口必须证明后续有效 PTS 帧也继续提交。
        PlaybackSnapshot advanced;
        waitInQt([&] {
            advanced = player.getPlaybackSnapshot();
            return advanced.request_id == request && advanced.state == PlaybackState::Playing &&
                advanced.current_seconds >= playing.current_seconds + 0.5 && advanced.stats.render_fps >= 8.0;
        }, 1800ms, "first video frame was submitted, but later frames stalled on the eight-second RTP offset");
        std::cout << "startup_video_rtp_offset_s=8 audio_advanced_s=" << advanced.current_seconds - playing.current_seconds
                  << " video_submit_fps=" << advanced.stats.render_fps << '\n';
    }
    if (native) {
        // 真实窗口可画频谱，但只有切回视频并执行 GL 绘制后才能出现首帧绘制里程碑。
        widget.setShowSpectrum(false);
        waitInQt([&] {
            const auto snapshot = player.getPlaybackSnapshot();
            return widget.firstDrawnAt(epoch) && snapshot.network_stats && snapshot.network_stats->first_video_draw_seconds &&
                   *snapshot.network_stats->first_video_draw_seconds >= *milestones.first_video_submit_seconds;
        }, 2s, "native GL video draw did not follow the video submission milestone");
        std::cout << "native_gl_video_draw_after_submission=1 spectrum_not_a_video_draw=1\n";
    }

    std::cout << "startup_mode=" << mode << " source_pts=" << playing.current_seconds
              << " first_packet_s=" << *milestones.first_packet_seconds
              << " first_pcm_s=" << milestones.first_pcm_seconds.value_or(-1.0)
              << " first_submit_s=" << milestones.first_video_submit_seconds.value_or(-1.0)
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()
              << " drawing_not_needed_for_playing=1\n";
    player.shutdown();
    waitInQt([&] { return !server.isHoldingConnection(); }, 1s, "live fixture peer did not observe disconnect");
    return 0;
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

// 用真实网络对话框和播放列表走一遍本地→直播→本地，检查显示、能力和数据库路由。
void testWindowMixedRtsp()
{
    using Access = MainWindowNetworkTestAccess;
    const VideoFixtures fixtures;
    RtspBlackhole server(RtspBlackhole::StallAt::VideoReadPacket, testRtspAddress(), RtspBlackhole::SetupTransport::Udp);
    TestWindow window;
    auto& view = *window.view;
    window.view->setInitialFiles({fixtures.first().u8string(), fixtures.second().u8string()});
    const auto first = MainWindowCloseTestAccess::snapshot(view).request_id;
    require(first > 0 && Access::controls(view).library_request == first, "local source did not enter media-library routing");
    waitInQt([&] {
        const auto snapshot = MainWindowCloseTestAccess::snapshot(view);
        const auto controls = Access::controls(view);
        return snapshot.request_id == first && snapshot.state == PlaybackState::Playing && snapshot.has_current_video_output &&
               snapshot.media_info.video_info.width == 64 && controls.seek_enabled && controls.speed_enabled;
    }, 2s, "initial local file did not enable seek and speed");
    Access::chooseSpeed(view, 6);
    require(std::abs(Access::preferredSpeed(view) - 2.0f) < 0.01f, "local speed combo did not request 2x");

    auto raw = server.url() + "?token=MediaStudioSecretWindowQuery";
    raw.insert(7, "MediaStudioSecretWindowUser:MediaStudioSecretWindowPassword@");
    // 命令行、拖放等本地入口的共用路由不能把带凭据的 URL 打开成文件。
    window.view->setInitialFiles({raw});
    require(MainWindowCloseTestAccess::snapshot(view).request_id == first &&
            !view.statusBar()->currentMessage().contains("MediaStudioSecret"), "local entry routed or displayed a network URL");

    bool masked = false;
    bool invalid_rejected = false;
    bool default_tcp = false;
    bool cleared_after_open = false;
    QTimer dialog_deadline;
    dialog_deadline.setSingleShot(true);
    QObject::connect(&dialog_deadline, &QTimer::timeout, &view, [&] {
        if (auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget())) active->reject();
    });
    dialog_deadline.start(1000);
    QTimer::singleShot(0, &view, [&] {
        auto* dialog = view.findChild<QDialog*>("networkMediaDialog");
        if (!dialog) return;
        auto* input = dialog->findChild<QLineEdit*>("networkUrlInput");
        auto* transport = dialog->findChild<QComboBox*>("rtspTransportSelector");
        auto* error = dialog->findChild<QLabel*>("networkUrlError");
        auto* buttons = dialog->findChild<QDialogButtonBox*>("networkDialogButtons");
        if (!input || !transport || !error || !buttons || !buttons->button(QDialogButtonBox::Open)) {
            dialog->reject();
            return;
        }
        auto* open = buttons->button(QDialogButtonBox::Open);
        default_tcp = transport->count() == 2 && transport->currentData().toInt() == static_cast<int>(RtspTransport::Tcp);
        input->setText("https://example.test/video?token=MediaStudioSecretInvalidDialog");
        open->click();
        invalid_rejected = dialog->isVisible() && !error->text().isEmpty() && !error->text().contains("MediaStudioSecret") &&
            MainWindowCloseTestAccess::snapshot(view).request_id == first;
        input->setText(QString::fromStdString(raw));
        masked = input->echoMode() == QLineEdit::Password && !input->displayText().contains("MediaStudioSecret");
        QObject::connect(dialog, &QDialog::finished, input, [&] { cleared_after_open = input->text().isEmpty(); });
        transport->setCurrentIndex(transport->findData(static_cast<int>(RtspTransport::Udp)));
        open->click();
    });
    require(QMetaObject::invokeMethod(&view, "openNetworkMedia", Qt::DirectConnection),
            "could not invoke production network dialog slot");
    dialog_deadline.stop();
    require(masked && invalid_rejected && default_tcp && cleared_after_open,
            "network dialog did not safely validate, mask, default to TCP or clear its address");

    const auto live_id = MainWindowCloseTestAccess::snapshot(view).request_id;
    require(live_id > first && Access::hasSafeLiveRow(view, raw) && Access::controls(view).library_request == 0,
            "RTSP source did not enter the safe mixed playlist route or reached media-library routing");
    PlaybackSnapshot live;
    waitInQt([&] {
        live = MainWindowCloseTestAccess::snapshot(view);
        const auto controls = Access::controls(view);
        return live.request_id == live_id && live.state == PlaybackState::Playing && live.has_current_video_output &&
               live.media_info.video_info.width == 32 && live.live_elapsed_seconds && *live.live_elapsed_seconds > 0.0 &&
               controls.duration == QString::fromUtf8(u8"直播") && controls.elapsed.startsWith(QString::fromUtf8(u8"已播放 ")) &&
               !controls.elapsed.endsWith("--:--") && controls.metadata.contains("32x32");
    }, 3s, "live source did not display video, elapsed time and protocol metadata");
    const auto live_controls = Access::controls(view);
    require(live.source_kind == PlaybackSourceKind::NetworkLive && live.protocol == PlaybackProtocol::Rtsp &&
            live.rtsp_transport == RtspTransport::Udp && server.setupTransport() == RtspBlackhole::SetupTransport::Udp &&
            !live.capabilities.can_seek && !live.capabilities.can_change_speed && !live.capabilities.duration_known &&
            !live_controls.seek_enabled && !live_controls.speed_enabled && live_controls.library_request == 0 &&
            live.display_name == "RTSP" && live.media_info.file_path.empty() && live.media_info.db_id < 0 &&
            !live_controls.metadata.contains("MediaStudioSecret") && !view.statusBar()->currentMessage().contains("MediaStudioSecret"),
            "live capabilities, transport, metadata, or library isolation differed from the RTSP source contract");

    Access::attemptLiveMutations(view);
    PlaybackSnapshot after_mutations;
    waitInQt([&] {
        after_mutations = MainWindowCloseTestAccess::snapshot(view);
        return after_mutations.live_elapsed_seconds && *after_mutations.live_elapsed_seconds > *live.live_elapsed_seconds + 0.1;
    }, 1s, "live clock did not continue after forbidden mutations");
    require(after_mutations.request_id == live.request_id && after_mutations.generation == live.generation &&
            !after_mutations.last_error && std::abs(Access::preferredSpeed(view) - 2.0f) < 0.01f,
            "live position/speed commands changed the session or local speed preference");

    require(QMetaObject::invokeMethod(&view, "prevFile", Qt::DirectConnection), "could not activate mixed-list previous action");
    const auto second = MainWindowCloseTestAccess::snapshot(view).request_id;
    require(second > live_id && Access::controls(view).library_request == second,
            "switching through the mixed playlist did not restore local database routing");
    waitInQt([&] {
        const auto snapshot = MainWindowCloseTestAccess::snapshot(view);
        const auto controls = Access::controls(view);
        return snapshot.request_id == second && snapshot.state == PlaybackState::Playing && snapshot.has_current_video_output &&
               snapshot.source_kind == PlaybackSourceKind::LocalFile && snapshot.media_info.video_info.width == 80 &&
               snapshot.capabilities.can_seek && snapshot.capabilities.can_change_speed && controls.seek_enabled &&
               controls.speed_enabled && controls.speed_index == 6 && !server.isHoldingConnection();
    }, 2s, "returning to the second file did not restore the saved controls or close the RTSP peer");
    require(std::abs(Access::preferredSpeed(view) - 2.0f) < 0.01f, "returned local file lost 2x speed");

    // 2x 播放约 0.6 秒即可推进 1.2 秒媒体时间，期限给帧和调度留出余量。
    const auto resumed_at = Clock::now();
    const auto position = MainWindowCloseTestAccess::snapshot(view).current_seconds;
    waitInQt([&] { return MainWindowCloseTestAccess::snapshot(view).current_seconds >= position + 1.2; },
             1s, "returned local video did not actually advance at the restored 2x speed");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - resumed_at).count();
    std::cout << "dialog_validation=1 mixed_file_rtsp_udp_file=1 local_speed_restored=1"
              << " local_video_1200_media_ms_elapsed=" << elapsed_ms << " live_library_route_skipped=1\n";
    closeWindow(window.view, second, "mixed-rtsp-local-playing");
}

// 列表删除当前源后，主窗口仍对活动直播暂停/继续，并在密集重接中正常关窗。
void testWindowLiveControls()
{
    using Fixture = RtspBlackhole;
    using Stall = Fixture::StallAt;
    Fixture server({Stall::VideoReadPacket, Stall::VideoReadPacket, Stall::OpenInput,
                    Stall::OpenInput, Stall::OpenInput, Stall::OpenInput}, testRtspAddress(), Fixture::SetupTransport::Tcp);
    TestWindow window;
    auto& view = *window.view;
    const auto source = createStableRtspSource(server.url(), RtspTransport::Tcp);
    MainWindowNetworkTestAccess::openListed(view, *source.source);
    PlaybackSnapshot first;
    waitInQt([&] {
        first = MainWindowCloseTestAccess::snapshot(view);
        return first.state == PlaybackState::Playing && first.has_current_video_output;
    }, 2s, "window did not play its listed live source");
    auto* return_button = view.findChild<QPushButton*>("btn_return_to_live");
    require(return_button && return_button->isEnabled(), "current live window did not offer return-to-live");
    MainWindowNetworkTestAccess::removeSelected(view);
    require(QMetaObject::invokeMethod(&view, "togglePlayPause", Qt::DirectConnection), "window pause action missing");
    waitInQt([&] {
        const auto paused = MainWindowCloseTestAccess::snapshot(view);
        return paused.state == PlaybackState::Paused && !paused.is_session_active && server.closedConnections() == 1 &&
            view.statusBar()->currentMessage().contains(QString::fromUtf8(u8"已断开直播"));
    }, 2s, "window pause did not disconnect and render settled paused status after removing its row");
    require(return_button->isEnabled() && QMetaObject::invokeMethod(&view, "togglePlayPause", Qt::DirectConnection),
            "removed live row prevented continue");
    PlaybackSnapshot resumed;
    waitInQt([&] {
        resumed = MainWindowCloseTestAccess::snapshot(view);
        return resumed.request_id > first.request_id && resumed.state == PlaybackState::Playing && resumed.has_current_video_output;
    }, 2s, "continue followed a stale playlist row instead of reconnecting the current source");
    return_button->click();
    waitInQt([&] { return server.acceptedConnections() == 3 && server.isHoldingConnection(); }, 2s,
             "actual return-to-live button did not reconnect current source");
    for (int i = 0; i < 4; ++i) return_button->click();
    const auto last = MainWindowCloseTestAccess::snapshot(view).request_id;
    require(last > resumed.request_id && MainWindowNetworkTestAccess::controls(view).library_request == 0,
            "repeated return-to-live failed to replace current source or entered library routing");
    closeWindow(window.view, last, "rapid-return-to-live");
    const auto accepted = server.acceptedConnections();
    remainsInQt([&] { return server.acceptedConnections() == accepted && !server.isHoldingConnection(); },
                180ms, "closed window reopened an obsolete live request");
    std::cout << "window_live_pause_continue_removed_row=1 return_button_reconnect=1 repeated_return_close=1\n";
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
        else if (mode == "window-mixed-rtsp") testWindowMixedRtsp();
        else if (mode == "window-live-controls") testWindowLiveControls();
        else if (mode == "live-pause-open" || mode == "live-pause-probe" || mode == "live-pause-read") testLivePauseStage(mode);
        else if (mode == "live-pause-av-tcp" || mode == "live-pause-av-udp" || mode.substr(0, 11) == "live-cycle-")
            return testLiveLifecycle(mode);
        else if (mode == "mixed-switch-tcp" || mode == "mixed-switch-udp") return testMixedSwitchStress(mode == "mixed-switch-udp");
        else if (mode == "live-failures-tcp" || mode == "live-failures-udp") testLiveFailures(mode == "live-failures-udp");
        else if (mode == "live-command-races") testLiveCommandRaces();
        else if (mode.substr(0, 11) == "live-start-") return testLiveStartup(mode);
        else if (mode == "widget-epochs") testWidgetEpochs();
        else throw std::runtime_error("unknown session test");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Session test failed: " << error.what() << '\n';
        return 1;
    }
}
