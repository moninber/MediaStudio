#include "LiveReceiveMonitor.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace std::chrono_literals;
using Monitor = LiveReceiveMonitor;
namespace {
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
Monitor::Clock::time_point at(std::chrono::milliseconds elapsed) { return Monitor::Clock::time_point{} + elapsed; }
LiveReceivedPacket video(std::optional<double> dts, std::uint64_t bytes = 100)
{
    return {LiveReferenceTrack::Video, bytes, false, true, dts, .04};
}
// 到达时间与媒体时间分别控制：大绝对 DTS、负 DTS、慢速及快送均不依赖真实睡眠。
void feed(Monitor& monitor, double rate = 1.0, double origin = 2700.0)
{
    for (int ms = 0; ms < 4000; ms += 40) monitor.recordPacket(video(origin + ms / 1000.0 * rate), at(std::chrono::milliseconds{ms}));
}
void testWindows()
{
    Monitor monitor; monitor.reset(at(0ms), LiveReferenceTrack::Video);
    monitor.recordPacket(video(2700, 100), at(100ms));
    monitor.recordPacket({LiveReferenceTrack::Audio, 200, false, true, 0, .02}, at(400ms));
    monitor.recordPacket(video(2700.8, 300), at(900ms));
    require(!monitor.snapshot(at(999ms)).application_bitrate_bps, "partial second was published as a full window");
    monitor.recordPacket(video(2700.9, 400), at(1000ms));
    const auto first = monitor.snapshot(at(1000ms));
    require(first.window_packets == 3 && first.window_payload_bytes == 600 && first.application_bitrate_bps == 4800 &&
        first.received_packets == 4 && first.received_payload_bytes == 1000 && first.sample_sequence == 1,
        "packet at the right boundary leaked backwards or totals/bit units differ");
    const auto repeated = monitor.snapshot(at(1900ms));
    require(repeated.application_bitrate_bps == first.application_bitrate_bps && repeated.sample_sequence == 1 &&
        std::abs(*repeated.sample_age_seconds - .9) < 1e-12, "UI polling altered the fixed window or produced a second observation");
    const auto second = monitor.snapshot(at(2000ms)), silent = monitor.snapshot(at(3000ms));
    require(second.window_packets == 1 && second.application_bitrate_bps == 3200 && silent.window_packets == 0 &&
        silent.application_bitrate_bps == 0 && silent.last_packet_age_seconds == 2 && silent.received_packets == 4,
        "blocked read did not become a zero receive window or changed cumulative counts");
    monitor.recordPacket(video(2710, 50), at(9300ms));
    require(monitor.snapshot(at(9500ms)).window_packets == 0 && monitor.snapshot(at(10000ms)).window_payload_bytes == 50,
        "ring wrap reused an old nonzero bucket");
    std::cout << "one_second_windows=1 polling_independent=1 boundary_packet=1 silence_zero=1 bounded_history_wrap=1\n";
}
void testProgress()
{
    for (const double rate : {.5, 1.0, 1.5}) {
        Monitor monitor; monitor.reset(at(0ms), LiveReferenceTrack::Video); feed(monitor, rate, -30.0);
        const auto stats = monitor.snapshot(at(4000ms));
        require(stats.timing_state == LiveTimingState::Ready && stats.media_progress_rate &&
            std::abs(*stats.media_progress_rate - rate) < 1e-9 && stats.relative_lag_seconds &&
            std::abs(*stats.relative_lag_seconds - (4.0 - 3.96 * rate)) < 1e-9,
            "DTS/wallclock ratio or signed relative growth was fabricated from PTS/playback time");
        if (rate < 1) require(*stats.relative_lag_seconds > 1.5, "slow input did not show growing relative lag");
        if (rate > 1) require(*stats.relative_lag_seconds < 0, "faster input was incorrectly clamped to zero lag");
    }
    Monitor lone; lone.reset(at(0ms), LiveReferenceTrack::Video); lone.recordPacket(video(12345), at(0ms));
    require(!lone.snapshot(at(2500ms)).media_progress_rate, "one isolated DTS invented a trustworthy zero-rate sample");
    std::cout << "media_rates=0.5,1.0,1.5 signed_lag=1 negative_absolute_DTS_valid=1 lone_sample_unavailable=1\n";
}
void testInvalid()
{
    for (int kind = 0; kind < 7; ++kind) {
        Monitor monitor; monitor.reset(at(0ms), LiveReferenceTrack::Video); feed(monitor);
        auto broken = video(2703.96);
        const LiveTimingState expected[] = {LiveTimingState::MissingDts, LiveTimingState::InvalidTimeBase,
            LiveTimingState::RepeatedDts, LiveTimingState::BackwardDts, LiveTimingState::TimestampJump,
            LiveTimingState::InvalidDts, LiveTimingState::InvalidDts};
        if (kind == 0) broken.dts_seconds.reset();
        if (kind == 1) broken.time_base_valid = false;
        if (kind == 3) broken.dts_seconds = 2703;
        if (kind == 4) broken.dts_seconds = 2800;
        if (kind == 5) broken.dts_seconds = std::numeric_limits<double>::quiet_NaN();
        if (kind == 6) broken.corrupt = true;
        monitor.recordPacket(broken, at(4000ms));
        const auto invalid = monitor.snapshot(at(4001ms));
        require(invalid.timing_state == expected[kind] && !invalid.media_progress_rate && !invalid.relative_lag_seconds &&
            invalid.timestamp_domain > 0 && invalid.received_packets == 101,
            "missing/repeated/backward/jump/invalid DTS kept a prior good measurement");
        monitor.recordPacket(video(5000), at(4100ms));
        require(monitor.snapshot(at(4101ms)).timing_state == LiveTimingState::WarmingUp &&
            !monitor.snapshot(at(6000ms)).media_progress_rate, "new timestamp baseline skipped two-second warm-up");
    }
    std::cout << "missing_invalid_repeat_backward_jump_corrupt=unavailable no_PTS_fallback=1 baseline_warmup=1\n";
}
void testSelection()
{
    Monitor monitor; monitor.reset(at(0ms), LiveReferenceTrack::Video); feed(monitor);
    monitor.recordPacket(video({}), at(4000ms));
    for (int ms = 4020; ms <= 5000; ms += 20) monitor.recordPacket({LiveReferenceTrack::Audio, 160, false, true, ms / 1000.0, .02}, at(std::chrono::milliseconds{ms}));
    const auto selected = monitor.snapshot(at(5000ms));
    require(selected.reference_track == LiveReferenceTrack::Video && selected.timing_state == LiveTimingState::MissingDts &&
        !selected.media_progress_rate && selected.last_packet_age_seconds == 0 && *selected.application_bitrate_bps > 0,
        "fresh audio concealed unavailable video DTS or receive metrics were tied to timestamp validity");
    const auto before = selected.received_packets;
    monitor.recordPacket({LiveReferenceTrack::None, 999, false, true, 1, {}}, at(5010ms));
    monitor.recordPacket(video(5000, 0), at(5011ms));
    auto corrupt = video(5000, 50); corrupt.corrupt = true; monitor.recordPacket(corrupt, at(5020ms));
    const auto after = monitor.snapshot(at(5020ms));
    require(after.received_packets == before + 1 && std::abs(*after.last_packet_age_seconds - .02) < 1e-12,
        "empty/unselected/corrupt payload accounting refreshed the wrong valid-packet age");
    monitor.reset(at(10000ms), LiveReferenceTrack::Audio);
    for (int ms = 0; ms < 4000; ms += 20) monitor.recordPacket({LiveReferenceTrack::Audio, 160, false, true, 2700 + ms / 1000.0, .02}, at(10000ms + std::chrono::milliseconds{ms}));
    const auto audio = monitor.snapshot(at(14000ms));
    require(audio.reference_track == LiveReferenceTrack::Audio && audio.media_progress_rate && std::abs(*audio.media_progress_rate - 1) < 1e-9,
        "audio-only input did not select audio DTS");
    std::cout << "video_reference_priority=1 audio_only_reference=1 payload_independent_of_DTS=1 empty_unselected_filtered=1\n";
}
void testStale()
{
    Monitor monitor; monitor.reset(at(0ms), LiveReferenceTrack::Video); feed(monitor);
    const auto stale = monitor.snapshot(at(7001ms));
    require(stale.timing_state == LiveTimingState::Stale && !stale.media_progress_rate && !stale.relative_lag_seconds &&
        stale.application_bitrate_bps == 0 && *stale.reference_dts_age_seconds > 3,
        "stale DTS remained eligible or blocked reads retained an old nonzero bitrate");
    monitor.recordPacket(video(2704), at(7100ms));
    const auto resumed = monitor.snapshot(at(7101ms));
    require(resumed.timing_state == LiveTimingState::WarmingUp && resumed.timestamp_domain > stale.timestamp_domain &&
        !resumed.relative_lag_seconds, "late resume reused the stale baseline as live lag evidence");
    std::cout << "stale_after_3s=1 silent_window_zero=1 resume_new_domain=1\n";
}
void testLifecycle()
{
    Monitor monitor; monitor.reset(at(0ms), LiveReferenceTrack::Video); feed(monitor);
    const auto before = monitor.snapshot(at(4000ms)); monitor.finish(at(4000ms));
    monitor.recordPacket(video(2704), at(4100ms));
    const auto frozen = monitor.snapshot(at(60000ms));
    require(!frozen.active && frozen.received_packets == before.received_packets &&
        frozen.last_packet_age_seconds == before.last_packet_age_seconds && frozen.sample_sequence == before.sample_sequence,
        "retired connection aged as an active receiver or accepted late data");
    monitor.reset(at(100000ms), LiveReferenceTrack::Audio);
    const auto reset = monitor.snapshot(at(100010ms));
    require(reset.active && reset.reference_track == LiveReferenceTrack::Audio && reset.received_packets == 0 &&
        reset.received_payload_bytes == 0 && reset.timestamp_domain == 0 && reset.sample_sequence == 0 &&
        !reset.application_bitrate_bps && !reset.last_packet_age_seconds && !reset.relative_lag_seconds,
        "new request/attempt inherited old totals, window or timestamp anchor");
    require(before.active && before.received_packets == 100 && before.media_progress_rate,
        "saved stats copy mutated after finish/reset");
    std::cout << "finish_freezes_history=1 no_late_packet=1 reset_window_totals_domain=1 by_value_snapshot=1\n";
}
}
int main(int argc, char** argv)
{
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "windows") testWindows(); else if (mode == "progress") testProgress();
        else if (mode == "invalid") testInvalid(); else if (mode == "selection") testSelection();
        else if (mode == "stale") testStale(); else if (mode == "lifecycle") testLifecycle();
        else throw std::runtime_error("unknown receive-monitor test");
        return 0;
    } catch (const std::exception& e) { std::cerr << "Receive monitor failed: " << e.what() << '\n'; return 1; }
}
