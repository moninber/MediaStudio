#include "LiveRecovery.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <type_traits>

extern "C" {
#include <libavutil/error.h>
}

namespace {
using namespace std::chrono_literals;
constexpr std::array RetryDelays{1s, 2s, 4s};
constexpr auto RecoveryLimit = 45s;
constexpr auto HealthyOutputPeriod = 30s;
constexpr auto MaximumObservationGap = 2s;

// 只对明确的永久失败终止重试。404、429、其他 4xx/5xx 可能是暂未发布或临时拒绝。
std::optional<LiveFailureReason> permanentReason(int code)
{
    if (code == AVERROR_HTTP_UNAUTHORIZED || code == AVERROR_HTTP_FORBIDDEN ||
        code == AVERROR(EACCES) || code == AVERROR(EPERM)) return LiveFailureReason::AccessDenied;
    if (code == AVERROR(ENOMEM)) return LiveFailureReason::ResourceExhausted;
    if (code == AVERROR_PROTOCOL_NOT_FOUND || code == AVERROR_DEMUXER_NOT_FOUND ||
        code == AVERROR_OPTION_NOT_FOUND || code == AVERROR(ENOSYS) ||
        code == AVERROR_PATCHWELCOME || code == AVERROR_EXPERIMENTAL) return LiveFailureReason::UnsupportedInput;
    if (code == AVERROR_DECODER_NOT_FOUND || code == AVERROR_FILTER_NOT_FOUND ||
        code == AVERROR_STREAM_NOT_FOUND) return LiveFailureReason::DecoderUnavailable;
    if (code == AVERROR(EINVAL) || code == AVERROR_HTTP_BAD_REQUEST) return LiveFailureReason::InvalidSource;
    if (code == AVERROR_EXIT) return LiveFailureReason::UnclassifiedInterruption;
    return {};
}
}

LiveFailure classifyLiveFailure(PlaybackSourceKind kind, const PlaybackError& error) noexcept
{
    return std::visit([kind](const auto& result) -> LiveFailure {
        const int code = result.ffmpegError;
        if (kind != PlaybackSourceKind::NetworkLive) {
            return {LiveFailureDisposition::None, LiveFailureReason::NotLive, code};
        }
        using Result = std::decay_t<decltype(result)>;
        bool cancelled, timeout, success;
        if constexpr (std::is_same_v<Result, MediaOpenResult>) {
            cancelled = result.status == MediaOpenStatus::Cancelled;
            timeout = result.status == MediaOpenStatus::Timeout;
            success = result.status == MediaOpenStatus::Opened;
        } else {
            cancelled = result.status == MediaReadStatus::Cancelled;
            timeout = result.status == MediaReadStatus::Timeout;
            success = result.status == MediaReadStatus::Packet || result.status == MediaReadStatus::Again;
        }
        // 可信的应用分类优先于原始码，例如最后一个缓冲包返回 0 时也可能已经超时。
        if (cancelled) return {LiveFailureDisposition::Cancelled, LiveFailureReason::UserCancelled, code};
        if (success) return {LiveFailureDisposition::None, LiveFailureReason::None, code};
        if constexpr (std::is_same_v<Result, MediaReadResult>) {
            if (result.status == MediaReadStatus::QueueFull) return classifyLiveFailure(kind, LiveFailureEvent::QueueLimit);
            if (result.status == MediaReadStatus::PacketTooLarge) return classifyLiveFailure(kind, LiveFailureEvent::PacketTooLarge);
        }
        if (timeout) {
            const auto reason = [&] {
                if constexpr (std::is_same_v<Result, MediaReadResult>) {
                    if (result.stage == MediaReadStage::InitialOutput || result.stage == MediaReadStage::AudioOutput ||
                        result.stage == MediaReadStage::VideoOutput) return LiveFailureReason::OutputTimeout;
                }
                return LiveFailureReason::IoTimeout;
            }();
            return {LiveFailureDisposition::Retryable, reason, code};
        }
        if (const auto reason = permanentReason(code)) return {LiveFailureDisposition::Fatal, *reason, code};
        if constexpr (std::is_same_v<Result, MediaOpenResult>) {
            if (result.stage == MediaOpenStage::Validation) {
                return {LiveFailureDisposition::Fatal, LiveFailureReason::InvalidSource, code};
            }
            if (result.stage == MediaOpenStage::InitializeDecoders) {
                return {LiveFailureDisposition::Fatal, LiveFailureReason::SetupFailed, code};
            }
        } else {
            if (result.status == MediaReadStatus::EndOfStream) {
                return {LiveFailureDisposition::Retryable, LiveFailureReason::LiveEof, code};
            }
        }
        if (code == AVERROR_EOF) return {LiveFailureDisposition::Retryable, LiveFailureReason::LiveEof, code};
        return {LiveFailureDisposition::Retryable, LiveFailureReason::ConnectionFailure, code};
    }, error);
}

LiveFailure classifyLiveFailure(PlaybackSourceKind kind, LiveFailureEvent event) noexcept
{
    if (kind != PlaybackSourceKind::NetworkLive) return {LiveFailureDisposition::None, LiveFailureReason::NotLive, 0};
    switch (event) {
        case LiveFailureEvent::OutputStalled:
            return {LiveFailureDisposition::Retryable, LiveFailureReason::OutputTimeout, 0};
        case LiveFailureEvent::QueueLimit:
            return {LiveFailureDisposition::Retryable, LiveFailureReason::QueueLimit, 0};
        case LiveFailureEvent::PacketTooLarge:
            return {LiveFailureDisposition::Fatal, LiveFailureReason::PacketTooLarge, 0};
    }
    return {LiveFailureDisposition::Fatal, LiveFailureReason::InvalidSource, 0};
}

void LiveRecoveryBudget::resetHealth() noexcept
{
    healthy_since_.reset();
    last_observation_.reset();
}

void LiveRecoveryBudget::stop(RecoveryStopReason reason) noexcept
{
    state_ = State::Stopped;
    stop_reason_ = reason;
    retry_at_.reset();
    resetHealth();
}

void LiveRecoveryBudget::cancel() noexcept
{
    stop(RecoveryStopReason::Cancelled);
}

RecoveryDecision LiveRecoveryBudget::next(RecoveryClock::time_point now)
{
    // 恢复后的正常输出不能在第 45 秒被强行停止；但未稳定满 30 秒前再次失败仍受原预算约束。
    if (state_ != State::Monitoring && state_ != State::Stopped && deadline_ && now >= *deadline_) {
        stop(RecoveryStopReason::DeadlineExceeded);
    }
    RecoveryDecision decision;
    decision.stop_reason = stop_reason_;
    decision.retries_started = retries_started_;
    decision.deadline = deadline_;
    switch (state_) {
        case State::Idle: break;
        case State::Stopped: decision.action = RecoveryAction::Stop; break;
        case State::Attempting: decision.action = RecoveryAction::InProgress; break;
        case State::Monitoring: decision.action = RecoveryAction::Monitor; break;
        case State::Backoff:
            decision.action = now >= *retry_at_ ? RecoveryAction::StartAttempt : RecoveryAction::Wait;
            decision.wake_at = std::min(*retry_at_, *deadline_);
            break;
    }
    return decision;
}

RecoveryDecision LiveRecoveryBudget::onFailure(LiveFailure failure, RecoveryClock::time_point now)
{
    if (failure.disposition == LiveFailureDisposition::None) return next(now);
    if (failure.disposition == LiveFailureDisposition::Cancelled) {
        last_failure_ = failure;
        cancel();
        return next(now);
    }
    if (state_ == State::Stopped) return next(now);
    if (failure.disposition == LiveFailureDisposition::Fatal || !automatic_enabled_) {
        last_failure_ = failure;
        stop(failure.disposition == LiveFailureDisposition::Fatal
            ? RecoveryStopReason::FatalFailure : RecoveryStopReason::Disabled);
        return next(now);
    }
    // 同一个故障的重复通知不能重排候选、延长退避或消耗新的重试次数。
    if (state_ == State::Backoff) return next(now);
    last_failure_ = failure;
    resetHealth();
    if (!deadline_) deadline_ = now + RecoveryLimit;
    if (now >= *deadline_) stop(RecoveryStopReason::DeadlineExceeded);
    else if (retries_started_ >= RetryDelays.size()) stop(RecoveryStopReason::AttemptsExhausted);
    else {
        state_ = State::Backoff;
        retry_at_ = now + RetryDelays[retries_started_];
    }
    return next(now);
}

bool LiveRecoveryBudget::startAttempt(RecoveryClock::time_point now)
{
    if (next(now).action != RecoveryAction::StartAttempt) return false;
    ++retries_started_;
    ++total_retries_started_;
    state_ = State::Attempting;
    retry_at_.reset();
    return true;
}

std::optional<RecoveryClock::time_point> LiveRecoveryBudget::operationDeadline(
    RecoveryClock::time_point now, std::chrono::milliseconds configured)
{
    if (configured.count() < 0 || next(now).action != RecoveryAction::InProgress) return {};
    // 先比较有界的剩余毫秒数，避免对极大的请求时长做纳秒加法而溢出。
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline_ - now);
    if (configured.count() == 0 || configured >= remaining) return deadline_;
    return now + configured;
}

bool LiveRecoveryBudget::outputsReady(RecoveryClock::time_point now)
{
    if (next(now).action != RecoveryAction::InProgress) return false;
    state_ = State::Monitoring;
    resetHealth();
    return true;
}

bool LiveRecoveryBudget::observeOutputs(RecoveryClock::time_point now, LiveOutputHealth health)
{
    if (state_ != State::Monitoring) return false;
    if (last_observation_ && (now < *last_observation_ || now - *last_observation_ > MaximumObservationGap)) {
        healthy_since_.reset();
    }
    last_observation_ = now;
    if (!health.required_tracks_outputting || !health.buffers_within_limits ||
        (health.media_progress_healthy && !*health.media_progress_healthy)) {
        healthy_since_.reset();
        return false;
    }
    if (!healthy_since_) healthy_since_ = now;
    if (now - *healthy_since_ < HealthyOutputPeriod) return false;
    // 恢复的是本轮可用预算；本次用户请求累计启动的自动重试次数继续保留。
    retries_started_ = 0;
    deadline_.reset();
    retry_at_.reset();
    state_ = State::Idle;
    resetHealth();
    return true;
}

void RecoveryWait::cancel()
{
    // 与 wait_until 的检查/入睡共用短锁，避免取消发生在两者之间时丢失唤醒。
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true, std::memory_order_release);
    changed_.notify_all();
}

RecoveryWaitResult RecoveryWait::waitUntil(RecoveryClock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_until(lock, deadline, [&] { return isCancelled(); })
        ? RecoveryWaitResult::Cancelled : RecoveryWaitResult::DeadlineReached;
}
