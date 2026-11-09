/**
 * @file PlayerController.h
 * @brief 播放控制器 - 管理播放状态、线程同步、播放列表
 * 
 * Qt版本：视频帧通过VideoWidget共享缓冲区传递，不直接调用OpenGL
 * 
 * 线程模型：
 * - Qt主线程：GUI + VideoWidget OpenGL渲染
 * - 视频解码线程：解码帧 → 传递给VideoWidget
 * - 音频解码线程：解码音频 → WASAPI播放 → 频谱数据传递
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

#include "MediaInfo.h"
#include "MediaDecoder.h"
#include "AudioOutput.h"
#include "PacketQueue.h"
#include "PlaybackSnapshot.h"

class VideoWidget;  // 前向声明，避免头文件依赖

class PlayerController {
public:
    PlayerController();
    ~PlayerController();

    /**
     * @brief 加载媒体文件
     * @param filename 文件路径
     * @return true=成功
     */
    bool loadFile(const std::string& filename);

    /** @brief 开始/继续播放 */
    void play();

    /** @brief 暂停播放 */
    void pause();

    /** @brief 停止播放 */
    void stop();

    /** @brief 跳转到指定时间 */
    void seek(double seconds);

    /** @brief 设置音量（0.0~1.0） */
    void setVolume(float volume);

    /** @brief 设置播放速度（0.25~4.0） */
    void setSpeed(float speed);

    /** @brief 切换视频特效 */
    void setEffect(VideoEffect effect);

    /** @brief 设置VideoWidget（用于帧数据传递） */
    void setVideoWidget(VideoWidget* widget) { video_widget_ = widget; }

    // 状态查询
    bool isPlaying() const { return playing_.load(); }
    bool isPaused() const { return paused_.load(); }
    double getCurrentTime() const { return current_time_.load(); }
    double getDuration() const { return duration_.load(); }
    float getVolume() const { return volume_.load(); }
    float getSpeed() const { return speed_.load(); }
    PlaybackStats getPlaybackStats() const;
    VideoEffect getEffect() const { return current_effect_; }

    /** @brief 按值读取界面所需状态和媒体信息，不接触 FFmpeg I/O 或声卡接口。 */
    PlaybackSnapshot getPlaybackSnapshot() const;

    /** @brief 仅回写指定本地加载请求的数据库字段，丢弃过期或不属于该源的结果。 */
    bool updateLocalLibraryInfo(std::uint64_t expected_request_id, const MediaInfo& info);

    /** @brief 切换播放/暂停 */
    void togglePlayPause();

    /** @brief 发布停止与 I/O 取消并唤醒队列，不等待播放线程退出。 */
    void stopAsync();

    /** @brief 切换可视化模式 */
    void toggleVisualizer() { show_visualizer_ = !show_visualizer_; }
    bool isVisualizerShown() const { return show_visualizer_; }

private:
    /** @brief 当前解复用循环，协调音视频工作线程和本次播放收尾。 */
    void playbackThread(std::uint64_t request_id);

    /** @brief 在状态锁内取消当前 I/O、更新停止标志并唤醒队列。 */
    void requestStopLocked();

    // 核心组件
    std::unique_ptr<MediaDecoder> decoder_;
    std::unique_ptr<AudioOutput> audio_output_;
    PacketQueue audio_packets_;
    PacketQueue video_packets_;
    VideoWidget* video_widget_;

    // 原子状态变量（线程安全，无需加锁）
    std::atomic<bool> playing_;
    std::atomic<bool> paused_;
    std::atomic<bool> stop_requested_;
    std::atomic<int>  session_id_;   // 每次play()递增，旧线程检测到不匹配则退出
    std::atomic<double> current_time_;
    std::atomic<double> duration_;
    std::atomic<float> volume_;
    std::atomic<float> speed_;
    std::atomic<bool> seek_requested_;
    std::atomic<double> pending_seek_seconds_;
    std::atomic<double> render_fps_;
    std::atomic<double> decode_fps_;
    std::atomic<double> av_diff_ms_;
    std::atomic<int> dropped_frames_total_;
    std::atomic<double> audio_buffer_seconds_{0.0};

    // 线程控制
    std::thread playback_thread_;
    std::recursive_mutex lifecycle_mutex_;
    std::mutex seek_mutex_;  // 播放线程内部执行seek/解码时互斥

    // 加载编号、取消令牌与元信息在同一短临界区发布，不持锁打开、解码或等待工作线程。
    mutable std::mutex snapshot_mutex_;
    PlaybackSnapshot snapshot_;
    std::shared_ptr<MediaIoControl> active_io_;
    std::uint64_t stop_epoch_ = 0;
    PlaybackState resume_state_ = PlaybackState::Playing;

    // 当前显示状态
    VideoEffect current_effect_;
    bool show_visualizer_;
};
