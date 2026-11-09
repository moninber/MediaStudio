#pragma once

#include <atomic>
#include <chrono>

enum class MediaIoStopReason { None, Cancelled, Timeout };

enum class MediaOpenStatus { Opened, Timeout, Cancelled, Error };
enum class MediaOpenStage { Validation, OpenInput, ProbeStreams, InitializeDecoders };

struct MediaOpenResult {
    MediaOpenStatus status;
    int ffmpegError; // 调用过 FFmpeg 时保留原始返回值；前置检查使用对应的 AVERROR。
    MediaOpenStage stage;
};

enum class MediaReadStatus { Packet, Again, EndOfStream, Timeout, Cancelled, Error };

struct MediaReadResult {
    MediaReadStatus status;
    int ffmpegError; // 分类与 FFmpeg 原始返回值分别保存。
};

// 用中断原因区分 FFmpeg 通用退出码，保留暂时无包、EOF 和其他错误。
MediaReadResult classifyMediaReadResult(
    int ffmpegResult, MediaIoStopReason stopReason = MediaIoStopReason::None) noexcept;

class MediaDecoder;

// 每次打开尝试使用独立对象。控制线程只发布取消，解码器管理截止时间。
class MediaIoControl {
public:
    // 取消一经发布就不会复位；下一次连接尝试需要新对象。
    void cancel() noexcept;
    // 查询是否由调用方主动取消，不把超时当作取消。
    bool isCancelled() const noexcept;

private:
    friend class MediaDecoder;
    using Clock = std::chrono::steady_clock;
    static constexpr auto NoDeadline = Clock::time_point::max().time_since_epoch().count();
    // 不允许实现以隐藏互斥锁代替回调中使用的原子读取。
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<Clock::duration::rep>::is_always_lock_free);

    // 按单调时钟设置当前操作期限，0 表示无期限。
    void armDeadline(std::chrono::milliseconds timeout) noexcept;
    // 收尾时为网络 I/O 设置期限，但不延长已有的期限。
    void limitDeadline(std::chrono::milliseconds timeout) noexcept;
    // 成功后撤销操作期限，避免消费队列中的空闲时间算入下一次读取。
    void clearDeadline() noexcept;
    // 只读取原子标志和时钟；同一操作取消优先于期限。
    MediaIoStopReason stopReason() const noexcept;
    // FFmpeg 在阻塞 I/O 期间轮询，回调不等待、分配资源或调用 Qt。
    static int interruptCallback(void* opaque) noexcept;

    std::atomic<bool> cancelled_{false};
    std::atomic<Clock::duration::rep> deadline_{NoDeadline};
};
