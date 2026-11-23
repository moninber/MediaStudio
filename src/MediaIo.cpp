#include "MediaIo.h"

#include <algorithm>
#include <cerrno>

extern "C" {
#include <libavutil/error.h>
}

// FFmpeg 可能对中断返回通用错误；用本次操作的取消/期限给出具体结果。
MediaReadResult classifyMediaReadResult(int ffmpegResult, MediaIoStopReason stopReason) noexcept
{
    MediaReadStatus status = MediaReadStatus::Error;
    if (stopReason == MediaIoStopReason::Cancelled) {
        status = MediaReadStatus::Cancelled;
    } else if (stopReason == MediaIoStopReason::Timeout || ffmpegResult == AVERROR(ETIMEDOUT)) {
        status = MediaReadStatus::Timeout;
    } else if (ffmpegResult >= 0) {
        status = MediaReadStatus::Packet;
    } else if (ffmpegResult == AVERROR(EAGAIN)) {
        status = MediaReadStatus::Again;
    } else if (ffmpegResult == AVERROR_EOF) {
        status = MediaReadStatus::EndOfStream;
    }
    return {status, ffmpegResult};
}

// 控制线程可直接发布取消，不需要解码锁或生命周期锁。
void MediaIoControl::cancel() noexcept
{
    cancelled_.store(true, std::memory_order_release);
}

// 只查询用户取消标志，和网络期限分开处理。
bool MediaIoControl::isCancelled() const noexcept
{
    return cancelled_.load(std::memory_order_acquire);
}

void MediaIoControl::beginInitialOutputDeadline(Clock::time_point deadline) noexcept
{
    initial_output_deadline_.store(deadline.time_since_epoch().count(), std::memory_order_release);
}

bool MediaIoControl::finishInitialOutput() noexcept
{
    if (isInitialOutputTimedOut()) return false;
    initial_output_deadline_.store(NoDeadline, std::memory_order_release);
    return true;
}

bool MediaIoControl::isInitialOutputTimedOut() const noexcept
{
    const auto deadline = initial_output_deadline_.load(std::memory_order_acquire);
    return deadline != NoDeadline && Clock::now().time_since_epoch().count() >= deadline;
}

void MediaIoControl::setRecoveryDeadline(Clock::time_point deadline) noexcept
{
    recovery_deadline_.store(deadline.time_since_epoch().count(), std::memory_order_release);
}

bool MediaIoControl::finishRecoveryDeadline() noexcept
{
    const auto deadline = recovery_deadline_.load(std::memory_order_acquire);
    if (isCancelled() || (deadline != NoDeadline && Clock::now().time_since_epoch().count() >= deadline)) return false;
    recovery_deadline_.store(NoDeadline, std::memory_order_release);
    return true;
}

void MediaIoControl::beginOutputWatchdogs(bool audio, bool video, Clock::time_point deadline) noexcept
{
    const auto ticks = deadline.time_since_epoch().count();
    audio_output_deadline_.store(audio ? ticks : NoDeadline, std::memory_order_release);
    video_output_deadline_.store(video ? ticks : NoDeadline, std::memory_order_release);
}

void MediaIoControl::noteAudioOutput(Clock::time_point deadline) noexcept
{
    const auto previous = audio_output_deadline_.load(std::memory_order_acquire);
    if (previous != NoDeadline && Clock::now().time_since_epoch().count() < previous) {
        audio_output_deadline_.store(deadline.time_since_epoch().count(), std::memory_order_release);
    }
}

void MediaIoControl::noteVideoOutput(Clock::time_point deadline) noexcept
{
    const auto previous = video_output_deadline_.load(std::memory_order_acquire);
    if (previous != NoDeadline && Clock::now().time_since_epoch().count() < previous) {
        video_output_deadline_.store(deadline.time_since_epoch().count(), std::memory_order_release);
    }
}

std::optional<MediaReadStage> MediaIoControl::liveDeadlineStage(Clock::time_point now) const noexcept
{
    const auto expired = [ticks = now.time_since_epoch().count()](const auto& value) {
        const auto at = value.load(std::memory_order_acquire);
        return at != NoDeadline && ticks >= at;
    };
    if (expired(recovery_deadline_)) return MediaReadStage::RecoveryBudget;
    if (expired(initial_output_deadline_)) return MediaReadStage::InitialOutput;
    if (expired(audio_output_deadline_)) return MediaReadStage::AudioOutput;
    if (expired(video_output_deadline_)) return MediaReadStage::VideoOutput;
    return {};
}

// 期限从实际开始操作时计时，不受系统时间调整影响。
void MediaIoControl::armDeadline(std::chrono::milliseconds timeout) noexcept
{
    if (timeout.count() <= 0) {
        clearDeadline();
        return;
    }
    deadline_.store((Clock::now() + timeout).time_since_epoch().count(), std::memory_order_release);
}

// 由同一 I/O 所有者更新；FFmpeg 回调只观察，不写入期限。
void MediaIoControl::limitDeadline(std::chrono::milliseconds timeout) noexcept
{
    if (timeout.count() <= 0) {
        return;
    }
    const auto limit = (Clock::now() + timeout).time_since_epoch().count();
    if (limit < deadline_.load(std::memory_order_acquire)) {
        deadline_.store(limit, std::memory_order_release);
    }
}

// URLContext 复制了回调，但 opaque 仍指向本对象；无需替换 FFmpeg 内部回调。
void MediaIoControl::beginCloseDeadline(std::chrono::milliseconds timeout) noexcept
{
    auto unset = NoDeadline;
    const auto deadline = std::min((Clock::now() + timeout).time_since_epoch().count(),
        recovery_deadline_.load(std::memory_order_acquire));
    close_deadline_.compare_exchange_strong(unset, deadline, std::memory_order_release);
}

// 撤销当前操作的计时，不清除取消标志。
void MediaIoControl::clearDeadline() noexcept
{
    deadline_.store(NoDeadline, std::memory_order_release);
}

// FFmpeg 没有回调或尚未开始读取时也可以检查相同的退出条件。
MediaIoStopReason MediaIoControl::stopReason() const noexcept
{
    if (isCancelled()) {
        return MediaIoStopReason::Cancelled;
    }
    const auto deadline = deadline_.load(std::memory_order_acquire);
    if (liveDeadlineStage() ||
        (deadline != NoDeadline && Clock::now().time_since_epoch().count() >= deadline)) {
        return MediaIoStopReason::Timeout;
    }
    return MediaIoStopReason::None;
}

// opaque 由 MediaDecoder 持有，直到 FFmpeg 完成关闭后才释放。
int MediaIoControl::interruptCallback(void* opaque) noexcept
{
    const auto* control = static_cast<const MediaIoControl*>(opaque);
    // 原请求仍然已取消，stopReason/isCancelled 的结果不变。只在关闭期间允许
    // FFmpeg 尝试发送 TEARDOWN；不恢复读包，不等待回应，过期后继续中断。
    const auto closing = control->close_deadline_.load(std::memory_order_acquire);
    if (closing != NoDeadline) {
        return Clock::now().time_since_epoch().count() >= closing ? 1 : 0;
    }
    return control->stopReason() != MediaIoStopReason::None ? 1 : 0;
}
