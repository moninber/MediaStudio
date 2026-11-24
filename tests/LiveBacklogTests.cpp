#include "LiveBacklog.h"
#include "LiveRecovery.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace std::chrono_literals;
using Clock = LiveBacklogPolicy::Clock;
namespace {
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
const Clock::time_point Origin{};
LiveReceiveStats sample(std::uint64_t seq, double lag = 2, double rate = .5)
{
    LiveReceiveStats s; s.active = true; s.reference_track = LiveReferenceTrack::Video;
    s.timing_state = LiveTimingState::Ready; s.sample_sequence = seq; s.sample_age_seconds = 0;
    s.reference_dts_age_seconds = .1; s.baseline_age_seconds = static_cast<double>(seq);
    s.media_progress_rate = rate; s.relative_lag_seconds = lag;
    return s;
}
void testGates()
{
    for (int mode = 0; mode < 3; ++mode) {
        LiveBacklogPolicy p; p.reset(mode == 0 ? std::optional<LiveBacklogSettings>{} : LiveBacklogSettings{}, mode != 1, Origin);
        bool requested = false;
        for (int i = 3; i < 10; ++i) requested |= p.observe(sample(i), Origin + std::chrono::seconds{i}).request_reconnect;
        require(requested == (mode == 2), "stable/automatic-off gate or enabled low-latency gate incorrect");
        if (mode < 2) require(p.stats().state == LiveBacklogState::Disabled, "disabled gate accumulated a candidate");
    }
    LiveBacklogPolicy p; p.reset(LiveBacklogSettings{}, true, Origin);
    require(!p.observe(sample(1), Origin + 1s).candidate_started && p.stats().reason == LiveBacklogReason::WarmingUp,
        "connection warmup was skipped by a synthetic Ready flag");
    std::cout << "stable_disabled=1 automatic_off_disabled=1 low_enabled=1 warmup_2s=1\n";
}
void testThresholds()
{
    LiveBacklogPolicy p; p.reset(LiveBacklogSettings{}, true, Origin);
    require(!p.observe(sample(3, 1.5, .5), Origin + 3s).candidate_started, "lag threshold must be strictly greater than 1500ms");
    require(!p.observe(sample(4, 1.6, .8), Origin + 4s).candidate_started, "rate exactly .8 must recover");
    require(p.observe(sample(5, 1.6, .799), Origin + 5s).candidate_started, "strictly slow growing lag did not start a candidate");
    for (int i = 0; i < 10; ++i) require(!p.observe(sample(5, 1.6, .799), Origin + 5500ms).request_reconnect,
        "duplicate polling produced additional observations");
    require(p.stats().observations == 1 && p.stats().confirmed_seconds == 0, "same window advanced candidate duration");
    auto fresh = sample(6, 1.8); fresh.reference_dts_age_seconds = 3;
    require(!p.observe(fresh, Origin + 6s).candidate_cleared, "DTS freshness exactly 3s was rejected");
    fresh.reference_dts_age_seconds = 3.001;
    require(p.observe(fresh, Origin + 6500ms).candidate_cleared && p.stats().reason == LiveBacklogReason::InvalidReference,
        "stale reference did not immediately cancel within the same window");
    require(!p.observe(sample(6), Origin + 6500ms).candidate_started, "a cleared window was evaluated twice");
    std::cout << "strict_thresholds=1 same_window_once=1 freshness_3s_boundary=1 invalid_immediate_clear=1\n";
}
void testSustained()
{
    LiveBacklogPolicy p; p.reset(LiveBacklogSettings{}, true, Origin);
    require(p.observe(sample(3), Origin + 3s).candidate_started, "candidate missing");
    p.observe(sample(4, 2.2), Origin + 4s);
    require(!p.observe(sample(5, 2.4), Origin + 5s).request_reconnect && p.stats().observations == 3 &&
        p.stats().confirmed_seconds == 2, "three observations spanning only two seconds triggered prematurely");
    require(p.observe(sample(6, 2.6), Origin + 6s).request_reconnect && p.stats().confirmed_seconds == 3 &&
        p.stats().observations >= 3, "continuous three-second candidate did not request a reconnect");
    require(!p.observe(sample(7, 3), Origin + 7s).request_reconnect, "terminal request repeated on another window");
    std::cout << "minimum_3s_and_3_observations=1 four_boundary_samples=1 single_request=1\n";
}
void testWithdrawal()
{
    for (int mode = 0; mode < 8; ++mode) {
        LiveBacklogPolicy p; p.reset(LiveBacklogSettings{}, true, Origin); p.observe(sample(3), Origin + 3s);
        auto s = sample(4, 2.2);
        if (mode == 0) s.relative_lag_seconds = 1.4;
        if (mode == 1) s.media_progress_rate = .8;
        if (mode == 2) s.relative_lag_seconds = 1.9;
        if (mode == 3) s.timing_state = LiveTimingState::MissingDts;
        if (mode == 4) s.timestamp_domain = 1;
        if (mode == 5) s.reference_track = LiveReferenceTrack::Audio;
        if (mode == 6) s.media_progress_rate = std::numeric_limits<double>::quiet_NaN();
        if (mode == 7) s.relative_lag_seconds.reset();
        const auto result = p.observe(s, Origin + 4s);
        require(result.candidate_cleared && !result.request_reconnect, "recovering/invalid/new-domain evidence did not revoke old candidate");
        if (mode == 4 || mode == 5) require(result.candidate_started && p.stats().observations == 1,
            "new baseline did not start an independent qualifying run");
    }
    LiveBacklogPolicy p; p.reset(LiveBacklogSettings{}, true, Origin); p.observe(sample(3), Origin + 3s);
    const auto gap = p.observe(sample(5, 2.4), Origin + 5s);
    require(gap.candidate_cleared && gap.candidate_started && p.stats().observations == 1 && p.stats().confirmed_seconds == 0,
        "missing second was backfilled as continuous valid observations");
    std::cout << "lag_fall_progress_recovery_reference_domain_clear=1 missing_window_new_run=1\n";
}
void testCancellation()
{
    LiveBacklogPolicy p; p.reset(LiveBacklogSettings{}, true, Origin); p.observe(sample(3), Origin + 3s);
    require(p.cancel(LiveBacklogReason::UserCancelled).candidate_cleared, "user cancellation did not revoke candidate");
    for (int i = 4; i < 10; ++i) require(!p.observe(sample(i), Origin + std::chrono::seconds{i}).candidate_started,
        "late observation revived a cancelled connection");
    p.reset(LiveBacklogSettings{}, true, Origin + 10s);
    require(p.observe(sample(3), Origin + 13s).candidate_started, "new connection did not clear sticky cancellation");
    require(p.stats().observations == 1 && p.stats().confirmed_seconds == 0 && !p.stats().last_clear_reason,
        "new connection inherited candidate duration or cancellation history");
    std::cout << "user_cancel_sticky=1 no_late_candidate=1 next_connection_resets=1\n";
}
void testMonitorStream()
{
    for (double rate : {.5, 1.0, 1.5}) {
        LiveReceiveMonitor monitor; monitor.reset(Origin, LiveReferenceTrack::Video);
        LiveBacklogPolicy p; p.reset(LiveBacklogSettings{}, true, Origin);
        int starts = 0, requests = 0;
        for (int ms = 0; ms <= 8000; ms += 40) {
            const auto now = Origin + std::chrono::milliseconds{ms};
            monitor.recordPacket({LiveReferenceTrack::Video, 100, false, true, 2700 + ms / 1000.0 * rate, .04}, now);
            const auto d = p.observe(monitor.snapshot(now), now);
            starts += d.candidate_started; requests += d.request_reconnect;
        }
        require(starts == (rate == .5 ? 1 : 0) && requests == (rate == .5 ? 1 : 0),
            "real monitor windows and policy did not distinguish sustained .5x input from healthy/fast input");
    }
    std::cout << "monitor_to_policy=1 slow_input_requests=1 normal_fast_no_request=1\n";
}
void testBudget()
{
    const auto failure = classifyLiveFailure(PlaybackSourceKind::NetworkLive, MediaReadResult{MediaReadStatus::LiveBacklog, 0});
    require(failure.disposition == LiveFailureDisposition::Retryable && failure.reason == LiveFailureReason::Backlog &&
        failure.ffmpeg_error == 0, "backlog event became a fabricated FFmpeg error or ordinary queue Full");
    LiveRecoveryBudget budget(true); auto now = Origin;
    for (int retry = 0; retry < 3; ++retry) {
        const auto failed = budget.onFailure(retry % 2 ? classifyLiveFailure(PlaybackSourceKind::NetworkLive, LiveFailureEvent::QueueLimit) : failure, now);
        require(failed.wake_at && budget.startAttempt(*failed.wake_at), "mixed backlog/Full budget did not permit the next bounded attempt");
        now = *failed.wake_at + 1s; require(budget.outputsReady(now), "actual output marker rejected"); now += 3s;
    }
    require(budget.onFailure(failure, now).stop_reason == RecoveryStopReason::AttemptsExhausted && budget.totalRetriesStarted() == 3,
        "backlog created a separate unlimited retry budget");
    LiveRecoveryBudget disabled(false);
    require(disabled.onFailure(failure, Origin).stop_reason == RecoveryStopReason::Disabled && disabled.totalRetriesStarted() == 0,
        "disabled recovery started a backlog connection");
    std::cout << "typed_backlog_reason=1 shared_queue_and_backlog_budget=1 extra_attempts_max3=1 disabled_no_attempt=1\n";
}
}
int main(int argc, char** argv)
{
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "gates") testGates(); else if (mode == "thresholds") testThresholds();
        else if (mode == "sustained") testSustained(); else if (mode == "withdrawal") testWithdrawal();
        else if (mode == "cancellation") testCancellation(); else if (mode == "monitor-stream") testMonitorStream();
        else if (mode == "budget") testBudget(); else throw std::runtime_error("unknown backlog test");
        return 0;
    } catch (const std::exception& e) { std::cerr << "Backlog test failed: " << e.what() << '\n'; return 1; }
}
