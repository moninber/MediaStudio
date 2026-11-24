#include "LiveRecovery.h"

#include <array>
#include <cerrno>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/error.h>
}

using namespace std::chrono_literals;
using TimePoint = RecoveryClock::time_point;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// 使用非零的单调时间起点；业务测试只推进数值，不真实等待重试或稳定输出时间。
const auto Origin = TimePoint{123s};
const auto Live = PlaybackSourceKind::NetworkLive;
LiveFailure recoverable()
{
    return classifyLiveFailure(Live, MediaReadResult{MediaReadStatus::Timeout, AVERROR_EXIT});
}

void testClassification()
{
    using Disposition = LiveFailureDisposition;
    using Reason = LiveFailureReason;
    struct Case { PlaybackError input; Disposition disposition; Reason reason; };
    const Case cases[]{
        {MediaOpenResult{MediaOpenStatus::Opened, 0, MediaOpenStage::InitializeDecoders}, Disposition::None, Reason::None},
        {MediaReadResult{MediaReadStatus::Packet, 0}, Disposition::None, Reason::None},
        {MediaReadResult{MediaReadStatus::Again, AVERROR(EAGAIN)}, Disposition::None, Reason::None},
        {MediaReadResult{MediaReadStatus::Cancelled, AVERROR(ETIMEDOUT)}, Disposition::Cancelled, Reason::UserCancelled},
        {MediaOpenResult{MediaOpenStatus::Cancelled, AVERROR_HTTP_FORBIDDEN, MediaOpenStage::OpenInput}, Disposition::Cancelled, Reason::UserCancelled},
        {MediaReadResult{MediaReadStatus::Timeout, 0}, Disposition::Retryable, Reason::IoTimeout},
        {MediaReadResult{MediaReadStatus::QueueFull, 0, MediaReadStage::AudioQueue}, Disposition::Retryable, Reason::QueueLimit},
        {MediaReadResult{MediaReadStatus::PacketTooLarge, 0, MediaReadStage::VideoQueue}, Disposition::Fatal, Reason::PacketTooLarge},
        {MediaReadResult{MediaReadStatus::Timeout, AVERROR_EXIT, MediaReadStage::InitialOutput}, Disposition::Retryable, Reason::OutputTimeout},
        {MediaOpenResult{MediaOpenStatus::Timeout, AVERROR_EXIT, MediaOpenStage::ProbeStreams}, Disposition::Retryable, Reason::IoTimeout},
        {MediaReadResult{MediaReadStatus::EndOfStream, AVERROR_EOF}, Disposition::Retryable, Reason::LiveEof},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_EOF, MediaOpenStage::OpenInput}, Disposition::Retryable, Reason::LiveEof},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR(EINVAL), MediaOpenStage::Validation}, Disposition::Fatal, Reason::InvalidSource},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR(ENOSYS), MediaOpenStage::Validation}, Disposition::Fatal, Reason::UnsupportedInput},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_PROTOCOL_NOT_FOUND, MediaOpenStage::OpenInput}, Disposition::Fatal, Reason::UnsupportedInput},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_DEMUXER_NOT_FOUND, MediaOpenStage::Validation}, Disposition::Fatal, Reason::UnsupportedInput},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_OPTION_NOT_FOUND, MediaOpenStage::OpenInput}, Disposition::Fatal, Reason::UnsupportedInput},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_DECODER_NOT_FOUND, MediaOpenStage::InitializeDecoders}, Disposition::Fatal, Reason::DecoderUnavailable},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_STREAM_NOT_FOUND, MediaOpenStage::InitializeDecoders}, Disposition::Fatal, Reason::DecoderUnavailable},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_INVALIDDATA, MediaOpenStage::InitializeDecoders}, Disposition::Fatal, Reason::SetupFailed},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_HTTP_UNAUTHORIZED, MediaOpenStage::OpenInput}, Disposition::Fatal, Reason::AccessDenied},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_HTTP_FORBIDDEN, MediaOpenStage::OpenInput}, Disposition::Fatal, Reason::AccessDenied},
        {MediaReadResult{MediaReadStatus::Error, AVERROR(EACCES)}, Disposition::Fatal, Reason::AccessDenied},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR(ENOMEM), MediaOpenStage::OpenInput}, Disposition::Fatal, Reason::ResourceExhausted},
        {MediaReadResult{MediaReadStatus::Error, AVERROR_EXIT}, Disposition::Fatal, Reason::UnclassifiedInterruption},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_HTTP_BAD_REQUEST, MediaOpenStage::OpenInput}, Disposition::Fatal, Reason::InvalidSource},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_HTTP_NOT_FOUND, MediaOpenStage::OpenInput}, Disposition::Retryable, Reason::ConnectionFailure},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_HTTP_TOO_MANY_REQUESTS, MediaOpenStage::OpenInput}, Disposition::Retryable, Reason::ConnectionFailure},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_HTTP_OTHER_4XX, MediaOpenStage::OpenInput}, Disposition::Retryable, Reason::ConnectionFailure},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_HTTP_SERVER_ERROR, MediaOpenStage::OpenInput}, Disposition::Retryable, Reason::ConnectionFailure},
        {MediaOpenResult{MediaOpenStatus::Error, AVERROR_INVALIDDATA, MediaOpenStage::ProbeStreams}, Disposition::Retryable, Reason::ConnectionFailure},
        {MediaReadResult{MediaReadStatus::Error, AVERROR(EIO)}, Disposition::Retryable, Reason::ConnectionFailure},
        {MediaReadResult{MediaReadStatus::Error, AVERROR(ECONNRESET)}, Disposition::Retryable, Reason::ConnectionFailure}
    };
    for (const auto& item : cases) {
        const auto actual = classifyLiveFailure(Live, item.input);
        require(actual.disposition == item.disposition && actual.reason == item.reason,
                "live recovery classification disagrees with the source/status/stage contract");
        require(actual.ffmpeg_error == std::visit([](const auto& value) { return value.ffmpegError; }, item.input),
                "failure classification changed the original FFmpeg code");
        for (const auto kind : {PlaybackSourceKind::LocalFile, PlaybackSourceKind::NetworkVod}) {
            const auto other = classifyLiveFailure(kind, item.input);
            require(other.disposition == Disposition::None && other.reason == Reason::NotLive,
                    "live EOF/retry rules leaked into local files or network VoD");
        }
    }
    require(classifyLiveFailure(Live, LiveFailureEvent::OutputStalled).disposition == Disposition::Retryable &&
            classifyLiveFailure(Live, LiveFailureEvent::QueueLimit).disposition == Disposition::Retryable &&
            classifyLiveFailure(Live, LiveFailureEvent::PacketTooLarge).disposition == Disposition::Fatal &&
            classifyLiveFailure(PlaybackSourceKind::NetworkVod, LiveFailureEvent::QueueLimit).disposition == Disposition::None,
            "output/queue events must not invent FFmpeg codes or prescribe VoD reconnection");
    std::cout << "classification_cases=" << std::size(cases) << " local_vod_isolation=1 raw_error_preserved=1\n";
}

void testSchedule()
{
    LiveRecoveryBudget budget(true);
    require(budget.next(Origin).action == RecoveryAction::None && !budget.startAttempt(Origin) &&
            !budget.outputsReady(Origin), "an ordinary connection consumed automatic recovery budget");
    const std::array delays{1s, 2s, 4s};
    auto now = Origin;
    for (unsigned int index = 0; index < delays.size(); ++index) {
        const auto decision = budget.onFailure(recoverable(), now);
        require(decision.action == RecoveryAction::Wait && decision.wake_at == now + delays[index] &&
                decision.deadline == Origin + 45s && decision.retries_started == index,
                "backoff sequence, deadline, or actual-start counter is wrong");
        require(!budget.startAttempt(now + delays[index] - 1ns), "attempt started before the backoff elapsed");
        const auto duplicate = budget.onFailure(recoverable(), now + 500ms);
        require(duplicate.wake_at == decision.wake_at && duplicate.deadline == decision.deadline &&
                budget.totalRetriesStarted() == index, "duplicate fault extended or spent recovery budget");
        now += delays[index];
        require(budget.next(now).action == RecoveryAction::StartAttempt && budget.startAttempt(now) &&
                !budget.startAttempt(now) && budget.retriesUsed() == index + 1 &&
                budget.totalRetriesStarted() == index + 1, "candidate/start distinction permitted duplicate retry accounting");
        // 打开成功或仍在收包都不足以结束恢复，等待所有所需轨道实际输出。
        budget.onFailure(classifyLiveFailure(Live,
            MediaOpenResult{MediaOpenStatus::Opened, 0, MediaOpenStage::InitializeDecoders}), now);
        require(budget.next(now).action == RecoveryAction::InProgress &&
                !budget.observeOutputs(now + 1s, {true, true, true}), "open success falsely restored retry budget");
        now += 1s;
    }
    auto stopped = budget.onFailure(recoverable(), now);
    require(stopped.action == RecoveryAction::Stop && stopped.stop_reason == RecoveryStopReason::AttemptsExhausted &&
            budget.totalRetriesStarted() == 3 && !budget.startAttempt(now + 1h), "fourth automatic retry was allowed");
    require(budget.onFailure(recoverable(), now + 2h).stop_reason == RecoveryStopReason::AttemptsExhausted,
            "an exhausted request revived without explicit user intent");

    // 三次都曾短暂出音画也不能循环获得“第一次”重试，原始 45 秒同样不续期。
    LiveRecoveryBudget unstable(true);
    unstable.onFailure(recoverable(), Origin);
    require(unstable.startAttempt(Origin + 1s) && unstable.outputsReady(Origin + 2s), "first unstable output failed");
    auto second = unstable.onFailure(recoverable(), Origin + 10s);
    require(second.wake_at == Origin + 12s && second.deadline == Origin + 45s &&
            unstable.startAttempt(Origin + 12s) && unstable.outputsReady(Origin + 13s),
            "short successful output reset the second backoff or deadline");
    auto third = unstable.onFailure(recoverable(), Origin + 20s);
    require(third.wake_at == Origin + 24s && third.deadline == Origin + 45s &&
            unstable.startAttempt(Origin + 24s) && unstable.outputsReady(Origin + 25s) &&
            unstable.onFailure(recoverable(), Origin + 26s).stop_reason == RecoveryStopReason::AttemptsExhausted,
            "repeated brief output allowed an unbounded recovery loop");

    LiveRecoveryBudget disabled(false);
    require(disabled.onFailure(recoverable(), Origin).stop_reason == RecoveryStopReason::Disabled &&
            disabled.totalRetriesStarted() == 0, "disabled automatic recovery still attempted a connection");
    LiveRecoveryBudget fatal(true);
    fatal.onFailure(recoverable(), Origin);
    require(fatal.onFailure(classifyLiveFailure(Live, LiveFailureEvent::PacketTooLarge), Origin + 100ms).stop_reason ==
            RecoveryStopReason::FatalFailure && fatal.totalRetriesStarted() == 0, "permanent failure left a pending retry");
    LiveRecoveryBudget fresh_user_request(true);
    require(fresh_user_request.onFailure(recoverable(), now).wake_at == now + 1s &&
            fresh_user_request.totalRetriesStarted() == 0, "explicit user request inherited old recovery accounting");
    std::cout << "backoff_seconds=1,2,4 maximum_extra_attempts=3 candidate_not_counted=1 disabled_and_fatal_stop=1\n";
}

void testDeadline()
{
    LiveRecoveryBudget budget(true);
    budget.onFailure(recoverable(), Origin);
    require(!budget.operationDeadline(Origin, 5s) && budget.startAttempt(Origin + 1s), "operation allowed outside an attempt");
    require(budget.operationDeadline(Origin + 1s, 5s) == Origin + 6s &&
            budget.operationDeadline(Origin + 42s, 5s) == Origin + 45s &&
            budget.operationDeadline(Origin + 44s, 10s) == Origin + 45s &&
            budget.operationDeadline(Origin + 44s, 0ms) == Origin + 45s &&
            budget.operationDeadline(Origin + 44s, std::chrono::milliseconds::max()) == Origin + 45s &&
            !budget.operationDeadline(Origin + 44s, -1ms), "open/probe/first-output operation exceeded remaining total time");
    require(budget.next(Origin + 45s).stop_reason == RecoveryStopReason::DeadlineExceeded &&
            !budget.operationDeadline(Origin + 45s, 5s) && !budget.outputsReady(Origin + 45s),
            "attempt or first output succeeded at/after the 45-second bound");

    LiveRecoveryBudget clipped_wait(true);
    clipped_wait.onFailure(recoverable(), Origin);
    clipped_wait.startAttempt(Origin + 1s);
    const auto waiting = clipped_wait.onFailure(recoverable(), Origin + 44500ms);
    require(waiting.action == RecoveryAction::Wait && waiting.wake_at == Origin + 45s &&
            !clipped_wait.startAttempt(Origin + 44999ms) &&
            clipped_wait.next(Origin + 45s).stop_reason == RecoveryStopReason::DeadlineExceeded &&
            clipped_wait.totalRetriesStarted() == 1, "late backoff outlived total budget or counted an unstarted candidate");

    // 成功输出期间不因旧恢复期限到达而停播，但短暂恢复不能获得新的 45 秒。
    LiveRecoveryBudget flapping(true);
    flapping.onFailure(recoverable(), Origin);
    flapping.startAttempt(Origin + 1s);
    require(flapping.outputsReady(Origin + 40s), "late but in-budget output was rejected");
    for (int second = 40; second <= 46; ++second) flapping.observeOutputs(Origin + std::chrono::seconds(second), {true, true, true});
    require(flapping.next(Origin + 46s).action == RecoveryAction::Monitor &&
            !flapping.operationDeadline(Origin + 46s, 5s), "normal output inherited an expired retry IO deadline");
    require(flapping.onFailure(recoverable(), Origin + 46s).stop_reason == RecoveryStopReason::DeadlineExceeded &&
            flapping.totalRetriesStarted() == 1, "brief output restarted the total recovery deadline");
    std::cout << "virtual_budget_seconds=45 exact_boundary_stops=1 operation_and_backoff_clipped=1 brief_output_does_not_reset=1\n";
}

void testHealth()
{
    for (const auto progress : {std::optional<bool>{}, std::optional<bool>{true}}) {
        LiveRecoveryBudget budget(true);
        budget.onFailure(recoverable(), Origin);
        budget.startAttempt(Origin + 1s);
        require(budget.outputsReady(Origin + 40s), "retry outputs failed to enter observation");
        for (int second = 40; second < 70; ++second) {
            require(!budget.observeOutputs(Origin + std::chrono::seconds(second), {true, progress, true}) &&
                    budget.retriesUsed() == 1, "budget restored before 30 seconds of continuous real output");
        }
        require(budget.observeOutputs(Origin + 70s, {true, progress, true}) && budget.retriesUsed() == 0 &&
                budget.totalRetriesStarted() == 1 && !budget.next(Origin + 70s).deadline,
                "continuous output did not restore budget without erasing cumulative retry count");
        const auto decision = budget.onFailure(recoverable(), Origin + 71s);
        require(decision.wake_at == Origin + 72s && decision.deadline == Origin + 116s &&
                budget.startAttempt(Origin + 72s) && budget.totalRetriesStarted() == 2,
                "new recovery episode did not receive a fresh budget after stable output");
    }
    for (const bool observation_gap : {false, true}) {
        LiveRecoveryBudget budget(true);
        budget.onFailure(recoverable(), Origin);
        budget.startAttempt(Origin + 1s);
        budget.outputsReady(Origin + 2s);
        budget.observeOutputs(Origin + 2s, {true, true, true});
        if (observation_gap) {
            require(!budget.observeOutputs(Origin + 32s, {true, true, true}), "unobserved time was mistaken for continuous output");
        } else {
            for (int second = 3; second <= 32; ++second) {
                require(!budget.observeOutputs(Origin + std::chrono::seconds(second), {true, false, true}),
                        "known slow media progression restored retry budget");
            }
            budget.observeOutputs(Origin + 32s, {true, true, true});
        }
        for (int second = 33; second < 62; ++second) {
            require(!budget.observeOutputs(Origin + std::chrono::seconds(second), {true, true, true}),
                    "interrupted health observation retained old stable duration");
        }
        require(budget.observeOutputs(Origin + 62s, {true, true, true}), "new uninterrupted observation did not restore budget");
    }
    LiveRecoveryBudget missing_track(true);
    missing_track.onFailure(recoverable(), Origin);
    missing_track.startAttempt(Origin + 1s);
    missing_track.outputsReady(Origin + 2s);
    for (int second = 2; second <= 40; ++second) {
        require(!missing_track.observeOutputs(Origin + std::chrono::seconds(second), {false, {}, true}),
                "inactive required track restored retry budget");
    }
    require(missing_track.retriesUsed() == 1, "missing output cleared the unstable retry history");
    LiveRecoveryBudget two_second_samples(true);
    two_second_samples.onFailure(recoverable(), Origin);
    two_second_samples.startAttempt(Origin + 1s);
    two_second_samples.outputsReady(Origin + 2s);
    for (int second = 2; second < 32; second += 2) {
        require(!two_second_samples.observeOutputs(Origin + std::chrono::seconds(second), {true, {}, true}),
                "two-second sampling restored health too early");
    }
    require(two_second_samples.observeOutputs(Origin + 32s, {true, {}, true}),
            "exactly two-second observation gaps were incorrectly treated as missing observations");
    LiveRecoveryBudget unknown_buffers(true);
    unknown_buffers.onFailure(recoverable(), Origin);
    unknown_buffers.startAttempt(Origin + 1s);
    unknown_buffers.outputsReady(Origin + 2s);
    for (int second = 2; second <= 40; ++second) {
        require(!unknown_buffers.observeOutputs(Origin + std::chrono::seconds(second), {true, {}}),
                "unverified buffering with missing timestamps restored retry budget");
    }
    std::cout << "stable_output_seconds=30 unavailable_timestamps_allowed=1 unsafe_buffer_or_slow_or_gap_or_missing_track_blocks_reset=1 cumulative_count_retained=1\n";
}

void testCancellation()
{
    LiveRecoveryBudget budget(true);
    budget.onFailure(recoverable(), Origin);
    budget.cancel();
    require(!budget.startAttempt(Origin + 1s) && budget.next(Origin + 1s).stop_reason == RecoveryStopReason::Cancelled &&
            budget.totalRetriesStarted() == 0, "cancellation at backoff boundary started an automatic connection");
    LiveRecoveryBudget user_cancelled(true);
    user_cancelled.onFailure(recoverable(), Origin);
    const auto cancelled = classifyLiveFailure(Live, MediaReadResult{MediaReadStatus::Cancelled, AVERROR_EXIT});
    require(user_cancelled.onFailure(cancelled, Origin + 1s).stop_reason == RecoveryStopReason::Cancelled &&
            user_cancelled.totalRetriesStarted() == 0 && !user_cancelled.startAttempt(Origin + 10s),
            "typed cancellation spent or revived a pending retry");
    RecoveryWait already_due;
    require(already_due.waitUntil(RecoveryClock::now() - 1s) == RecoveryWaitResult::DeadlineReached,
            "expired wait did not return immediately");
    RecoveryWait pre_cancelled;
    pre_cancelled.cancel();
    require(pre_cancelled.waitUntil(RecoveryClock::now() + 1h) == RecoveryWaitResult::Cancelled && pre_cancelled.isCancelled(),
            "pre-cancelled wait lost sticky user intent");

    // 反复覆盖取消早于/恰好遇到线程入睡的竞争；每轮都先 join 再断言，不 detach。
    for (int round = 0; round < 64; ++round) {
        RecoveryWait wait;
        std::promise<void> entering;
        auto entered = entering.get_future();
        RecoveryWaitResult result = RecoveryWaitResult::DeadlineReached;
        std::thread worker([&] {
            entering.set_value();
            result = wait.waitUntil(RecoveryClock::now() + 2s);
        });
        entered.wait();
        const auto at = RecoveryClock::now();
        wait.cancel();
        worker.join();
        require(result == RecoveryWaitResult::Cancelled && RecoveryClock::now() - at < 1s,
                "cancellation was lost between predicate check and wait entry");
    }
    RecoveryWait shared;
    std::array<RecoveryWaitResult, 8> results{};
    std::array<std::promise<void>, 8> entering;
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < results.size(); ++index) {
        workers.emplace_back([&, index] {
            entering[index].set_value();
            results[index] = shared.waitUntil(RecoveryClock::now() + 2s);
        });
    }
    for (auto& signal : entering) signal.get_future().wait();
    shared.cancel();
    for (auto& worker : workers) worker.join();
    for (const auto result : results) require(result == RecoveryWaitResult::Cancelled, "cancel failed to notify all waiters");
    std::cout << "cancel_wait_entry_races=64 waiters_woken=8 no_real_backoff_sleeps=1 cancelled_candidate_not_counted=1\n";
}

// 在同一套原子截止状态上推进观察时间，验证活动轨道独立、首输出和总预算的优先级。
void testOutputWatchdogs()
{
    const auto now = RecoveryClock::now();
    MediaIoControl video_stalled;
    video_stalled.beginOutputWatchdogs(true, true, now + 10s);
    video_stalled.noteAudioOutput(now + 20s);
    require(!video_stalled.liveDeadlineStage(now + 9s) &&
        video_stalled.liveDeadlineStage(now + 10s) == MediaReadStage::VideoOutput,
        "audio writes hid a stalled selected video track");
    MediaIoControl audio_stalled;
    audio_stalled.beginOutputWatchdogs(true, true, now + 10s);
    audio_stalled.noteVideoOutput(now + 20s);
    require(audio_stalled.liveDeadlineStage(now + 10s) == MediaReadStage::AudioOutput,
        "video submissions hid a stalled selected audio track");
    MediaIoControl inactive;
    inactive.beginOutputWatchdogs(false, false, now - 1s);
    require(!inactive.liveDeadlineStage(now + 1h), "absent tracks acquired an output deadline");
    MediaIoControl late_frame;
    late_frame.beginOutputWatchdogs(false, true, now - 1s);
    late_frame.noteVideoOutput(now + 10s);
    require(late_frame.liveDeadlineStage() == MediaReadStage::VideoOutput, "late output revived an expired connection");
    MediaIoControl budget;
    budget.beginOutputWatchdogs(true, true, now + 10s);
    budget.beginInitialOutputDeadline(now + 5s);
    budget.setRecoveryDeadline(now + 3s);
    require(budget.liveDeadlineStage(now + 3s) == MediaReadStage::RecoveryBudget,
        "ongoing startup outlived the total recovery budget");
    require(budget.finishRecoveryDeadline() && budget.liveDeadlineStage(now + 5s) == MediaReadStage::InitialOutput &&
        budget.finishInitialOutput() && budget.liveDeadlineStage(now + 10s) == MediaReadStage::AudioOutput,
        "successful startup removed steady output protection or left the old recovery deadline armed");
    MediaIoControl expired;
    expired.setRecoveryDeadline(now - 1s);
    require(!expired.finishRecoveryDeadline() && expired.liveDeadlineStage() == MediaReadStage::RecoveryBudget,
        "already-expired recovery deadline was cleared by a late first output");
    std::cout << "independent_output_watchdogs=audio,video inactive_tracks_ignored=1 total_budget_caps_startup=1 late_output_cannot_revive=1\n";
}
}

int main(int argc, char** argv)
{
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "classification") testClassification();
        else if (mode == "schedule") testSchedule();
        else if (mode == "deadline") testDeadline();
        else if (mode == "health") testHealth();
        else if (mode == "cancellation") testCancellation();
        else if (mode == "watchdogs") testOutputWatchdogs();
        else throw std::runtime_error("expected classification, schedule, deadline, health, cancellation or watchdogs");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Recovery test failed: " << error.what() << '\n';
        return 1;
    }
}
