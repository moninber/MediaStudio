#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "MediaDecoder.h"
#include "PacketQueue.h"
#include "PlaybackSnapshot.h"

class VideoWidget;

// 管理播放命令和当前连接。GUI 投递命令，supervisor 收尾并回收音视频线程。
class PlayerController {
public:
    PlayerController();
    ~PlayerController();

    // 新请求优先：立即取消旧 I/O，替换尚未开始的打开命令。
    std::uint64_t requestOpen(const PlaybackSource& source);
    std::uint64_t stopAsync();
    // 关窗先封闭命令入口并取消会话，GUI 保持事件循环，等待后台收尾。
    void requestShutdown();
    bool isShutdownComplete() const noexcept;
    // 由对象所有者调用，确保 supervisor 已 join 后再销毁控制器。
    void shutdown();

    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void setVolume(float volume);
    void setSpeed(float speed);
    void setEffect(VideoEffect effect);

    // 在提交首个播放命令之前绑定控件，控件须存活到后台收尾完成或 shutdown() 返回。
    void setVideoWidget(VideoWidget* widget);

    bool isPlaying() const { return playing_.load(); }
    bool isPaused() const { return paused_.load(); }
    double getCurrentTime() const { return current_time_.load(); }
    double getDuration() const { return duration_.load(); }
    float getVolume() const { return volume_.load(); }
    float getSpeed() const { return speed_.load(); }
    VideoEffect getEffect() const { return current_effect_.load(); }

    // 返回播放状态和元信息的值副本，调用者无需接触媒体 I/O 或声卡。
    PlaybackSnapshot getPlaybackSnapshot() const;
    bool updateLocalLibraryInfo(std::uint64_t expected_request_id, const MediaInfo& info);

    void toggleVisualizer();
    bool isVisualizerShown() const { return show_visualizer_.load(); }

private:
    struct OpenRequest {
        PlaybackSource source;
        PlaybackEpoch epoch;
        std::shared_ptr<MediaIoControl> io;
    };

    // supervisor 独占打开、读取、seek 与关闭，旧连接收尾后才执行新命令。
    void supervisorLoop();
    std::optional<PlaybackError> runRequest(const OpenRequest& request, VideoWidget* video);
    std::optional<MediaReadResult> demuxAndPlay(const OpenRequest& request, VideoWidget* video);
    void cancelCurrentLocked();
    void resetSnapshotLocked(std::uint64_t request_id, PlaybackState state);
    void setPausedLocked(bool paused);
    PlaybackEpoch currentEpochLocked() const;

    MediaDecoder decoder_;
    PacketQueue audio_packets_;
    PacketQueue video_packets_;

    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<double> current_time_{0.0};
    std::atomic<double> duration_{0.0};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> speed_{1.0f};
    std::atomic<bool> seek_requested_{false};
    std::atomic<VideoEffect> current_effect_{VideoEffect::None};
    std::atomic<bool> show_visualizer_{false};

    // 命令、取消状态与快照一起发布。媒体 I/O 和线程 join 在锁外执行。
    mutable std::mutex snapshot_mutex_;
    std::condition_variable command_changed_;
    std::optional<OpenRequest> pending_open_;
    std::optional<double> pending_seek_;
    PlaybackSnapshot snapshot_;
    std::optional<std::chrono::steady_clock::time_point> live_output_started_at_;
    std::shared_ptr<MediaIoControl> active_io_;
    VideoWidget* video_widget_ = nullptr;
    bool supervisor_busy_ = false;
    bool shutting_down_ = false;
    std::atomic<bool> shutdown_complete_{false};  // 仅 supervisor 在完成最后一次资源访问后发布。
    PlaybackState resume_state_ = PlaybackState::Playing;

    std::mutex codec_mutex_;  // 解码、排空和刷新互斥，解复用读包不持此锁。
    std::thread supervisor_;
};
