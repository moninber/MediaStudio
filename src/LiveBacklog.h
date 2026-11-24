#pragma once

#include "LiveReceiveMonitor.h"

// 阈值属于直播预设，显式带单位；策略不修改队列上限、倍速或解码参考帧。
struct LiveBacklogSettings {
    std::chrono::milliseconds lag_threshold{1500};
    double progress_limit = .8;
    std::chrono::seconds sustained_for{3};
    unsigned int minimum_observations = 3;
    std::chrono::seconds max_dts_age{3};
    std::chrono::seconds warmup{2};
};
enum class LiveBacklogState { Disabled, Waiting, Candidate, ReconnectRequested };
enum class LiveBacklogReason { None, StablePreset, AutomaticDisabled, WarmingUp, InvalidReference,
    SampleGap, BaselineChanged, LagBelowThreshold, ProgressRecovered, LagFalling, UserCancelled, ConnectionEnded };
struct LiveBacklogStats {
    bool enabled = false;
    LiveBacklogState state = LiveBacklogState::Disabled;
    LiveBacklogReason reason = LiveBacklogReason::StablePreset;
    std::optional<LiveBacklogReason> last_clear_reason;
    unsigned int observations = 0;
    double confirmed_seconds = 0.0;
    std::uint64_t sample_sequence = 0;
    std::uint64_t timestamp_domain = 0;
};
struct LiveBacklogDecision {
    bool candidate_started = false;
    bool candidate_cleared = false;
    bool request_reconnect = false;
};

// 纯策略，无线程/I/O/锁；控制器沿用快照锁串行驱动，明确用户取消也能在同一锁内撤销候选。
class LiveBacklogPolicy {
public:
    using Clock = LiveReceiveMonitor::Clock;
    void reset(std::optional<LiveBacklogSettings> settings, bool automatic, Clock::time_point started);
    LiveBacklogDecision observe(const LiveReceiveStats& receive, Clock::time_point now);
    LiveBacklogDecision cancel(LiveBacklogReason reason);
    LiveBacklogStats stats() const { return stats_; }

private:
    std::optional<LiveBacklogSettings> settings_;
    Clock::time_point started_{};
    LiveBacklogStats stats_;
    std::optional<Clock::time_point> candidate_since_;
    std::optional<std::uint64_t> last_sequence_, domain_;
    LiveReferenceTrack reference_ = LiveReferenceTrack::None;
    std::optional<double> previous_lag_;
    bool cancelled_ = false;
};
