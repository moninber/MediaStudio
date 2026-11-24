#include "LiveBacklog.h"

#include <cmath>

// 每个连接重新建策略；稳定模式与关闭自动选项都不发主动重接请求。
void LiveBacklogPolicy::reset(std::optional<LiveBacklogSettings> settings, bool automatic, Clock::time_point started)
{
    *this = {};
    settings_ = settings;
    started_ = started;
    stats_.enabled = settings.has_value() && automatic;
    stats_.state = stats_.enabled ? LiveBacklogState::Waiting : LiveBacklogState::Disabled;
    stats_.reason = !settings ? LiveBacklogReason::StablePreset
        : !automatic ? LiveBacklogReason::AutomaticDisabled : LiveBacklogReason::WarmingUp;
}

// 只把尚在累计的候选计为撤销，已经发出的请求不重复计数；用户取消仍可终止其后续重连。
LiveBacklogDecision LiveBacklogPolicy::cancel(LiveBacklogReason reason)
{
    LiveBacklogDecision result;
    if (reason == LiveBacklogReason::UserCancelled || reason == LiveBacklogReason::ConnectionEnded) cancelled_ = true;
    if (!stats_.enabled || (stats_.state == LiveBacklogState::ReconnectRequested && reason != LiveBacklogReason::UserCancelled)) return result;
    result.candidate_cleared = stats_.state == LiveBacklogState::Candidate;
    if (result.candidate_cleared) stats_.last_clear_reason = reason;
    stats_.state = LiveBacklogState::Waiting;
    stats_.reason = reason;
    stats_.observations = 0;
    stats_.confirmed_seconds = 0.0;
    candidate_since_.reset();
    previous_lag_.reset();
    return result;
}

// 同一完整秒窗最多推进一次。无效参考可立即撤销，缺失的秒窗绝不补算为连续有效观测。
LiveBacklogDecision LiveBacklogPolicy::observe(const LiveReceiveStats& receive, Clock::time_point now)
{
    LiveBacklogDecision result;
    if (!stats_.enabled || cancelled_ || stats_.state == LiveBacklogState::ReconnectRequested) return result;
    const auto clear = [&](LiveBacklogReason reason) {
        result.candidate_cleared |= cancel(reason).candidate_cleared;
    };
    stats_.sample_sequence = receive.sample_sequence;
    stats_.timestamp_domain = receive.timestamp_domain;
    if (domain_ && (*domain_ != receive.timestamp_domain || reference_ != receive.reference_track)) clear(LiveBacklogReason::BaselineChanged);
    domain_ = receive.timestamp_domain;
    reference_ = receive.reference_track;
    const bool same = last_sequence_ && receive.sample_sequence == *last_sequence_;
    const bool gap = last_sequence_ && receive.sample_sequence != *last_sequence_ + 1;
    const bool backward = last_sequence_ && receive.sample_sequence < *last_sequence_;
    if (receive.sample_sequence && (!last_sequence_ || receive.sample_sequence > *last_sequence_)) last_sequence_ = receive.sample_sequence;

    if (receive.timing_state == LiveTimingState::WarmingUp || receive.timing_state == LiveTimingState::Waiting) {
        clear(LiveBacklogReason::WarmingUp); return result;
    }
    const auto finite = [](const std::optional<double>& value) { return value && std::isfinite(*value); };
    if (!receive.active || receive.reference_track == LiveReferenceTrack::None || receive.timing_state != LiveTimingState::Ready ||
        !finite(receive.media_progress_rate) || *receive.media_progress_rate < 0.0 || !finite(receive.relative_lag_seconds) ||
        !finite(receive.reference_dts_age_seconds) || *receive.reference_dts_age_seconds < 0.0 ||
        *receive.reference_dts_age_seconds > std::chrono::duration<double>(settings_->max_dts_age).count() ||
        !finite(receive.sample_age_seconds) || *receive.sample_age_seconds < 0.0 || *receive.sample_age_seconds >= 1.0 ||
        !finite(receive.baseline_age_seconds)) {
        clear(LiveBacklogReason::InvalidReference); return result;
    }
    // 两个秒数相减只容忍纳秒级浮点舍入，不扩大预热/积压阈值。
    if (*receive.baseline_age_seconds - *receive.sample_age_seconds + 1e-9 < std::chrono::duration<double>(settings_->warmup).count()) {
        clear(LiveBacklogReason::WarmingUp); return result;
    }
    // 直接用连接起点和窗口号定位边界，避免 GUI/读包调度抖动改变“连续三秒”的长度。
    if (now < started_ || receive.sample_sequence == 0 || backward ||
        receive.sample_sequence != static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - started_).count())) {
        clear(LiveBacklogReason::SampleGap); return result;
    }
    if (same) return result;
    if (gap) clear(LiveBacklogReason::SampleGap);
    if (*receive.relative_lag_seconds <= std::chrono::duration<double>(settings_->lag_threshold).count()) {
        clear(LiveBacklogReason::LagBelowThreshold); return result;
    }
    if (*receive.media_progress_rate >= settings_->progress_limit) {
        clear(LiveBacklogReason::ProgressRecovered); return result;
    }
    if (previous_lag_ && *receive.relative_lag_seconds < *previous_lag_ - 1e-9) {
        clear(LiveBacklogReason::LagFalling); return result;
    }
    previous_lag_ = receive.relative_lag_seconds;
    const auto sample_at = started_ + std::chrono::seconds{static_cast<std::int64_t>(receive.sample_sequence)};
    if (stats_.state != LiveBacklogState::Candidate) {
        stats_.state = LiveBacklogState::Candidate;
        stats_.reason = LiveBacklogReason::None;
        candidate_since_ = sample_at;
        stats_.observations = 0;
        result.candidate_started = true;
    }
    ++stats_.observations;
    stats_.confirmed_seconds = std::chrono::duration<double>(sample_at - *candidate_since_).count();
    if (sample_at - *candidate_since_ >= settings_->sustained_for && stats_.observations >= settings_->minimum_observations) {
        stats_.state = LiveBacklogState::ReconnectRequested;
        result.request_reconnect = true;
    }
    return result;
}
