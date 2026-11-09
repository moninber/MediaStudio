#pragma once

#include <chrono>
#include <string>

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
    // 检查地址与源类型的基本匹配，不在此阶段探测网络或文件。
    bool isValid() const noexcept;

private:
    // 由三种工厂构造，避免调用方自行拼装不一致的源类型和显示名。
    PlaybackSource(PlaybackSourceKind kind, std::string address, std::string display_name,
                   LiveSourceOptions options = {});

    PlaybackSourceKind kind_;
    std::string address_;
    std::string display_name_;
    LiveSourceOptions live_options_;
};

enum class PauseBehavior { KeepPosition, DisconnectLive, FreezeVod };
enum class EndOfStreamBehavior { Drain, RecoverLive };
enum class QueueFullBehavior { LegacyThrottle, ReconnectLive, Backpressure };
enum class ReconnectPosition { None, LiveEdge, LastPlayedPosition };

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
