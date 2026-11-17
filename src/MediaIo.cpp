#include "MediaIo.h"

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
    if (isInitialOutputTimedOut() ||
        (deadline != NoDeadline && Clock::now().time_since_epoch().count() >= deadline)) {
        return MediaIoStopReason::Timeout;
    }
    return MediaIoStopReason::None;
}

// opaque 由 MediaDecoder 持有，直到 FFmpeg 完成关闭后才释放。
int MediaIoControl::interruptCallback(void* opaque) noexcept
{
    const auto* control = static_cast<const MediaIoControl*>(opaque);
    return control->stopReason() != MediaIoStopReason::None ? 1 : 0;
}
