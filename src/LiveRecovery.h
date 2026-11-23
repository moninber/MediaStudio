#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

#include "MediaIo.h"
#include "PlaybackSource.h"

// 业务层判定是否值得重试，原始 FFmpeg 错误码仍单独保存，不解析可能含地址的错误文本。
enum class LiveFailureDisposition { None, Cancelled, Fatal, Retryable };
enum class LiveFailureReason {
    None, NotLive, UserCancelled, InvalidSource, UnsupportedInput, AccessDenied,
    DecoderUnavailable, ResourceExhausted, SetupFailed, UnclassifiedInterruption,
    ConnectionFailure, IoTimeout, LiveEof, OutputTimeout, QueueLimit, PacketTooLarge
};

struct LiveFailure {
    LiveFailureDisposition disposition = LiveFailureDisposition::None;
    LiveFailureReason reason = LiveFailureReason::None;
    int ffmpeg_error = 0;
};

// 输出失活和队列限界由后续会话接入产生事件，不能伪造 FFmpeg 错误码。
enum class LiveFailureEvent { OutputStalled, QueueLimit, PacketTooLarge };
LiveFailure classifyLiveFailure(PlaybackSourceKind kind, const PlaybackError& error) noexcept;
LiveFailure classifyLiveFailure(PlaybackSourceKind kind, LiveFailureEvent event) noexcept;

using RecoveryClock = std::chrono::steady_clock;
enum class RecoveryAction { None, Wait, StartAttempt, InProgress, Monitor, Stop };
enum class RecoveryStopReason { None, Cancelled, Disabled, FatalFailure, AttemptsExhausted, DeadlineExceeded };

struct RecoveryDecision {
    RecoveryAction action = RecoveryAction::None;
    RecoveryStopReason stop_reason = RecoveryStopReason::None;
    unsigned int retries_started = 0;
    std::optional<RecoveryClock::time_point> wake_at;
    std::optional<RecoveryClock::time_point> deadline;
};

struct LiveOutputHealth {
    bool required_tracks_outputting = false;
    // 有可信推进指标时必须健康；时间戳不可信用空值，不能因此永远禁止恢复预算。
    std::optional<bool> media_progress_healthy;
    // 调用方核对本连接的缓冲限界；没有证据时保持 false，不能只看 Playing 状态。
    bool buffers_within_limits = false;
};

// 只由 supervisor 驱动的策略状态机，不打开连接、不访问 Qt/声卡；每个用户请求建立新对象。
// 所有时间显式传入单调时钟值，测试可以推进虚拟时间而无需等待 45 秒。
class LiveRecoveryBudget {
public:
    explicit LiveRecoveryBudget(bool automatic_enabled) : automatic_enabled_(automatic_enabled) {}

    RecoveryDecision onFailure(LiveFailure failure, RecoveryClock::time_point now);
    RecoveryDecision next(RecoveryClock::time_point now);
    // 调用方在创建连接的命令边界再次核对当前请求及取消状态；仅真正开始时计数。
    bool startAttempt(RecoveryClock::time_point now);
    // 必须由所需轨道的实际输出触发；打开/探测成功不算恢复，也不清空预算。
    bool outputsReady(RecoveryClock::time_point now);
    // 至少每秒观察输出，超过 2 秒的采样空窗或不健康输出都重新累计连续 30 秒。
    bool observeOutputs(RecoveryClock::time_point now, LiveOutputHealth health);
    void cancel() noexcept;

    // 只裁剪重试中的打开、探测和首输出期限，不给恢复后的正常读包套旧预算。
    // configured=0 表示只受总预算约束；负值或当前不在尝试中时不允许启动操作。
    std::optional<RecoveryClock::time_point> operationDeadline(
        RecoveryClock::time_point now, std::chrono::milliseconds configured);
    unsigned int retriesUsed() const noexcept { return retries_started_; }
    std::uint64_t totalRetriesStarted() const noexcept { return total_retries_started_; }
    std::optional<LiveFailure> lastFailure() const noexcept { return last_failure_; }

private:
    enum class State { Idle, Backoff, Attempting, Monitoring, Stopped };
    void stop(RecoveryStopReason reason) noexcept;
    void resetHealth() noexcept;

    const bool automatic_enabled_;
    State state_ = State::Idle;
    RecoveryStopReason stop_reason_ = RecoveryStopReason::None;
    unsigned int retries_started_ = 0;
    std::uint64_t total_retries_started_ = 0;
    std::optional<RecoveryClock::time_point> deadline_;
    std::optional<RecoveryClock::time_point> retry_at_;
    std::optional<RecoveryClock::time_point> healthy_since_;
    std::optional<RecoveryClock::time_point> last_observation_;
    std::optional<LiveFailure> last_failure_;
};

enum class RecoveryWaitResult { DeadlineReached, Cancelled };

// 请求独享的退避取消对象。GUI 取消只唤醒这个短锁，不等待网络 I/O 或生命周期锁。
// I/O 中断仍使用 MediaIoControl；本对象不在 FFmpeg 的回调中使用。
class RecoveryWait {
public:
    void cancel();
    bool isCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    RecoveryWaitResult waitUntil(RecoveryClock::time_point deadline);

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
};
