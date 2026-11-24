#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

enum class LiveReferenceTrack { None, Audio, Video };
enum class LiveTimingState { Waiting, WarmingUp, Ready, MissingDts, InvalidTimeBase,
                             InvalidDts, RepeatedDts, BackwardDts, TimestampJump, Stale };

// 只描述解复用返回的选中 AVPacket payload，不包括网络/容器头，不是网卡流量或端到端延迟。
struct LiveReceiveStats {
    bool active = false;
    std::uint64_t received_packets = 0;
    std::uint64_t received_payload_bytes = 0;
    std::optional<std::uint64_t> window_packets;
    std::optional<std::uint64_t> window_payload_bytes;
    std::optional<double> application_bitrate_bps;
    std::uint64_t sample_sequence = 0; // 完整秒窗编号；重复读取同一窗口不产生新观测。
    std::optional<double> sample_age_seconds;
    std::optional<double> last_packet_age_seconds;
    LiveReferenceTrack reference_track = LiveReferenceTrack::None;
    LiveTimingState timing_state = LiveTimingState::Waiting;
    std::uint64_t timestamp_domain = 0;
    std::optional<double> reference_dts_age_seconds;
    std::optional<double> media_progress_rate;
    std::optional<double> relative_lag_seconds;
};

// 适配层先用打开时缓存的 time_base 换算；DTS 缺失绝不以 PTS 冒充可信推进。
struct LiveReceivedPacket {
    LiveReferenceTrack track = LiveReferenceTrack::None;
    std::uint64_t payload_bytes = 0;
    bool corrupt = false;
    bool time_base_valid = true;
    std::optional<double> dts_seconds;
    std::optional<double> duration_seconds;
};

// 控制器已有的快照锁保护所有调用；本类无锁、线程、I/O、AVPacket 引用或动态增长的历史。
// 显式传入单调时间，按固定一秒窗口生成值副本，UI 刷新频率不改变采样边界。
class LiveReceiveMonitor {
public:
    using Clock = std::chrono::steady_clock;
    void reset(Clock::time_point started, LiveReferenceTrack reference);
    void recordPacket(const LiveReceivedPacket& packet, Clock::time_point now);
    LiveReceiveStats snapshot(Clock::time_point now) const;
    // 会话撤销时冻结最后观测，后续 UI 不把历史误报成仍在接收；新连接 reset 清空全部字段。
    void finish(Clock::time_point now);

private:
    struct Point { Clock::time_point at; double dts; std::uint64_t domain; };
    struct Bucket {
        std::int64_t index = -1;
        std::uint64_t bytes = 0;
        std::uint64_t packets = 0;
        std::optional<Point> reference;
    };
    // 只需最近一秒及其左边界；八个槽为三秒新鲜度/两秒预热留余量，静默窗口隐式为零。
    static constexpr std::size_t HistorySize = 8;
    void invalidate(LiveTimingState reason);
    std::optional<Point> pointBefore(Clock::time_point boundary) const;
    std::optional<Clock::time_point> started_;
    bool active_ = false;
    LiveReferenceTrack reference_ = LiveReferenceTrack::None;
    std::array<Bucket, HistorySize> buckets_{};
    std::uint64_t packets_ = 0, bytes_ = 0, domain_ = 0;
    std::optional<Clock::time_point> last_packet_at_;
    std::optional<Clock::time_point> last_event_at_;
    std::optional<Point> anchor_, latest_;
    std::optional<double> normal_step_;
    LiveTimingState timing_ = LiveTimingState::Waiting;
    LiveReceiveStats frozen_;
};
