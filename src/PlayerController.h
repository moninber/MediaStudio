#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "AudioOutput.h"
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
    // 窗口销毁输出控件前调用，等待 supervisor 正常退出。
    void shutdown();

    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void setVolume(float volume);
    void setSpeed(float speed);
    void setEffect(VideoEffect effect);

    // 在提交首个播放命令之前绑定控件，控件须存活到 shutdown() 返回。
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

    void toggleVisualizer() { show_visualizer_ = !show_visualizer_.load(); }
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
    AudioOutput audio_output_;
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
    std::shared_ptr<MediaIoControl> active_io_;
    VideoWidget* video_widget_ = nullptr;
    bool supervisor_busy_ = false;
    bool shutting_down_ = false;
    PlaybackState resume_state_ = PlaybackState::Playing;

    std::mutex seek_mutex_;  // 解码与 seek 互斥，格式上下文的锁归属在 1.3 收束。
    std::thread supervisor_;
};
