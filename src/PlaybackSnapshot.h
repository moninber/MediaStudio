#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "MediaInfo.h"
#include "MediaIo.h"
#include "LiveRecovery.h"
#include "PlaybackSource.h"

// 界面可观察到的播放状态；后续接入网络时继续使用同一组状态。
enum class PlaybackState {
    Idle,
    Opening,
    Buffering,
    Playing,
    Draining,
    Paused,
    Reconnecting,
    Stopping,
    Failed,
    Ended
};

// 沿用 V1 的统计口径：render_fps 计算提交给控件的帧，不代表实际绘制帧数。
struct PlaybackStats {
    double render_fps = 0.0;
    double decode_fps = 0.0;
    double av_diff_ms = 0.0;
    int dropped_frames_total = 0;
    int audio_queue_packets = 0;
    int video_queue_packets = 0;
    double audio_buffer_seconds = 0.0;
};

// 网络观测尚未采样时使用空值，不把未采集的数据显示为零码率或零包龄。
struct NetworkPlaybackStats {
    std::optional<double> application_bitrate_bps;
    std::optional<double> last_packet_age_seconds;
    std::uint64_t reconnect_attempts = 0;
    unsigned int recovery_retries_used = 0;
    bool auto_reconnect_enabled = false;
    LivePreset preset = LivePreset::Stable;
    std::optional<RtspUdpReceiveSettings> udp_receive;
    std::optional<LiveFailureReason> last_failure_reason;
    std::optional<RecoveryStopReason> recovery_stop_reason;
    std::optional<double> retry_in_seconds;
    double attempt_started_seconds = 0.0; // 当前连接相对用户请求的起点。
    std::optional<double> input_ready_seconds;
    std::optional<double> last_audio_output_age_seconds;
    std::optional<double> last_video_output_age_seconds;
    PacketQueueStats audio_queue;
    PacketQueueStats video_queue;
    // 按显式用户请求累计，跨自动连接保留；与同步丢帧/损坏帧分别计数。
    std::uint64_t queue_full_events = 0;
    std::uint64_t oversized_packet_events = 0;
    std::uint64_t audio_clock_reanchors = 0; // 本连接内因可信音频 PTS 跳变而重置设备时钟，不计作重连。
    std::optional<PacketQueueLimit> last_queue_limit;
    // 各阶段仅记首次，从当前连接尝试开始计时，自动重建时重置。
    std::optional<double> first_packet_seconds;
    std::optional<double> first_pcm_seconds;
    std::optional<double> first_video_submit_seconds;
    std::optional<double> first_video_draw_seconds;
    std::uint64_t corrupt_frames_skipped = 0;  // 与音画同步丢帧分开统计。
};

// 供界面按值获取的播放快照：仅包含可显示信息，不持有解码器、队列或原始网络地址。
struct PlaybackSnapshot {
    std::uint64_t request_id = 0;
    std::uint64_t connection_attempt_id = 0;
    std::uint64_t generation = 0;
    PlaybackSourceKind source_kind = PlaybackSourceKind::LocalFile;
    PlaybackProtocol protocol = PlaybackProtocol::Unknown;
    std::optional<RtspTransport> rtsp_transport;
    std::string display_name;
    PlaybackState state = PlaybackState::Idle;
    PlaybackCapabilities capabilities;
    bool is_playing = false;
    bool is_paused = false;
    bool is_session_active = false; // 仍在打开或占用会话资源，含旧线程及解码器收尾，不等同于有网络连接。
    bool has_current_video_output = false;  // 当前标签已有控件接收的帧，静态留帧不计入。
    double current_seconds = 0.0;
    double duration_seconds = 0.0;
    std::optional<double> live_elapsed_seconds;  // 从本连接的首次输出起计，不使用上游 PTS。
    PlaybackStats stats;
    MediaInfo media_info;
    std::optional<AudioBufferReport> audio_output; // 最近初始化结果，按连接重置；暂停/失败可保留历史。
    std::optional<PlaybackError> last_error;
    std::optional<NetworkPlaybackStats> network_stats;
};
