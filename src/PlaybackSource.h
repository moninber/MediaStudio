#pragma once

#include <chrono>
#include <optional>
#include <string>
#include "PacketQueueTypes.h"
#include "AudioBufferSettings.h"
#include "LiveBacklog.h"

// 源类型由调用方显式选择，不根据扩展名或“是否网络”猜测播放行为。
enum class PlaybackSourceKind { LocalFile, NetworkLive, NetworkVod };
enum class PlaybackProtocol { File, Rtsp, Rtmp, Srt, Http, Https, Unknown };
enum class RtspTransport { Tcp, Udp };
enum class LivePreset { Stable, LowLatency };

struct LiveSourceOptions {
    RtspTransport rtsp_transport = RtspTransport::Tcp;
    LivePreset preset = LivePreset::Stable;
    bool auto_reconnect = true;
};

// SRT URL 的单位为微秒；只缓存可展示的数字，不持有 streamid、口令或原始查询。
struct SrtInputSettings {
    std::optional<std::chrono::microseconds> latency;
    std::optional<std::chrono::microseconds> receive_latency;
    std::optional<std::chrono::microseconds> peer_latency;
    bool latency_from_url = false;
};

class PlaybackSource {
public:
    // 本地文件保留原始路径；显示名缺省时取路径末段。
    static PlaybackSource localFile(std::string path, std::string display_name = {});
    // 直播参数与本地、点播隔离；网络显示名不会包含原始 URL。
    static PlaybackSource networkLive(std::string url, LiveSourceOptions options = {});
    // 建立 HTTP/HTTPS 点播契约，打开及恢复能力在后续阶段接入。
    static PlaybackSource networkVod(std::string url);

    // 返回显式源类型与传输协议；两者分别决定播放和 I/O 行为。
    PlaybackSourceKind kind() const noexcept { return kind_; }
    PlaybackProtocol protocol() const noexcept;
    // 原始地址仅传给输入层，可能含凭据或签名，不用于日志、快照或数据库。
    const std::string& address() const noexcept { return address_; }
    // 供界面及快照使用；网络源仅显示固定协议名称。
    const std::string& displayName() const noexcept { return display_name_; }
    // 仅直播源提供直播选项，避免点播误读直播预设。
    const LiveSourceOptions* liveOptions() const noexcept;
    // URL 只在建立播放项时解析；播放策略在各线程只复制已校验的数字。
    const SrtInputSettings* srtOptions() const noexcept { return srt_options_ ? &*srt_options_ : nullptr; }
    // 返回创建时的地址验证结果，不再次解析或探测网络和文件。
    bool isValid() const noexcept { return valid_ && !address_.empty(); }

private:
    // 由三种工厂构造，避免调用方自行拼装不一致的源类型和显示名。
    PlaybackSource(PlaybackSourceKind kind, std::string address, std::string display_name, bool valid,
                   LiveSourceOptions options = {}, std::optional<SrtInputSettings> srt_options = {});

    PlaybackSourceKind kind_;
    std::string address_;
    std::string display_name_;
    bool valid_;
    LiveSourceOptions live_options_;
    std::optional<SrtInputSettings> srt_options_;
};

enum class LiveSourceError { None, UnsupportedProtocol, InvalidAddress, UnsupportedSrtMode, InvalidSrtOption };

// 两套 RTSP 预设共用校验结果，错误由调用方翻译为不含地址的提示。
struct LiveSourceValidation {
    std::optional<PlaybackSource> source;
    LiveSourceError error;
};
// 保留 RTSP 兼容入口；公开直播入口共用校验，但不让旧 RTSP 工厂接受其他协议。
using RtspSourceError = LiveSourceError;
using RtspSourceValidation = LiveSourceValidation;
LiveSourceValidation createLiveSource(std::string url, LiveSourceOptions options = {});
RtspSourceValidation createRtspSource(std::string url, LiveSourceOptions options = {});

RtspSourceValidation createStableRtspSource(std::string url, RtspTransport transport, bool auto_reconnect = true);

enum class PauseBehavior { KeepPosition, DisconnectLive, FreezeVod };
enum class EndOfStreamBehavior { Drain, RecoverLive };
enum class QueueFullBehavior { LegacyThrottle, ReconnectLive, Backpressure };
enum class ReconnectPosition { None, LiveEdge, LastPlayedPosition };

struct RtspUdpReceiveSettings {
    std::chrono::microseconds max_delay{0};
    int reorder_packets = 0;
};

// 由 supervisor 在 FFmpeg 打开后核对 AVOption，按值交给 GUI；不是协商后延迟或全部链路缓冲。
struct LiveInputReport {
    std::optional<std::chrono::milliseconds> rtmp_buffer;
    std::optional<SrtInputSettings> srt;
    std::optional<std::chrono::microseconds> probe_analysis;
};

// 一套直播预设的唯一数值来源；传输层、队列和音频线程只消费按值配置。
struct LivePresetSettings {
    std::chrono::milliseconds open_timeout;
    std::chrono::milliseconds probe_timeout;
    std::chrono::milliseconds read_timeout;
    std::chrono::milliseconds initial_output_timeout;
    std::chrono::milliseconds output_stall_timeout;
    PacketQueueLimits audio_queue;
    PacketQueueLimits video_queue;
    AudioBufferRequest audio_buffer;
    RtspUdpReceiveSettings rtsp_udp;
    // 低延迟直播减少帧率估算帧数，避免探测缓存集中吐包顶满 750 ms 队列；仍保留轨道探测和探测包。
    std::optional<int> fps_probe_frames;
    std::optional<LiveBacklogSettings> backlog;
    std::chrono::milliseconds rtmp_buffer;
    std::chrono::microseconds srt_latency;
    // FLV/MPEG-TS 分析的媒体时长，与整个探测操作的五秒墙钟期限不同。
    std::chrono::microseconds stream_probe_analysis;
};

const LivePresetSettings& livePresetSettings(LivePreset preset) noexcept;

// 描述各类源应采用的策略。EOF 排空、队列限界和重接由后续播放流程接入。
struct PlaybackPolicy {
    PauseBehavior pause = PauseBehavior::KeepPosition;
    EndOfStreamBehavior end_of_stream = EndOfStreamBehavior::Drain;
    QueueFullBehavior queue_full = QueueFullBehavior::LegacyThrottle;
    ReconnectPosition reconnect_position = ReconnectPosition::None;
    // 每次操作的期限；0 表示保留原有无期限行为。
    std::chrono::milliseconds open_timeout{0};
    std::chrono::milliseconds probe_timeout{0};
    std::chrono::milliseconds read_timeout{0};
    PacketQueueLimits audio_queue_limits;
    PacketQueueLimits video_queue_limits;
    AudioBufferRequest audio_buffer;
    std::chrono::milliseconds initial_output_timeout{0};
    std::chrono::milliseconds output_stall_timeout{0};
    // 仅 RTSP/UDP 使用，不把重排参数套给 TCP、RTMP、SRT 或本地文件。
    std::optional<RtspUdpReceiveSettings> rtsp_udp;
    // 仅直播低延迟使用，三种直播协议共用；不改变文件或点播的探测设置。
    std::optional<int> fps_probe_frames;
    std::optional<LiveBacklogSettings> backlog;
    std::optional<std::chrono::milliseconds> rtmp_buffer;
    std::optional<SrtInputSettings> srt;
    std::optional<std::chrono::microseconds> stream_probe_analysis;
};

struct PlaybackCapabilities {
    bool duration_known = false;
    bool can_seek = false;
    bool can_change_speed = false;
    bool can_return_to_live = false;
    bool can_resume_at_position = false;
};

// 按源类型选择操作策略和期限，不改变本地文件的 I/O 参数。
PlaybackPolicy playbackPolicy(const PlaybackSource& source) noexcept;
// 为已经识别的源生成初始能力。点播高级能力必须经后续实际探测才能开放。
PlaybackCapabilities initialPlaybackCapabilities(PlaybackSourceKind kind, bool duration_known) noexcept;
