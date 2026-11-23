#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <variant>

enum class MediaIoStopReason { None, Cancelled, Timeout };

enum class MediaOpenStatus { Opened, Timeout, Cancelled, Error };
enum class MediaOpenStage { Validation, OpenInput, ProbeStreams, InitializeDecoders };

struct MediaOpenResult {
    MediaOpenStatus status;
    int ffmpegError; // 调用过 FFmpeg 时保留原始返回值；前置检查使用对应的 AVERROR。
    MediaOpenStage stage;
};

enum class MediaReadStatus { Packet, Again, EndOfStream, Timeout, Cancelled, Error };
enum class MediaReadStage { ReadPacket, InitialOutput, AudioOutput, VideoOutput, RecoveryBudget };

struct MediaReadResult {
    MediaReadStatus status;
    int ffmpegError; // 分类与 FFmpeg 原始返回值分别保存。
    MediaReadStage stage = MediaReadStage::ReadPacket;
};

// 保留操作阶段和原始错误码，恢复策略与快照共用，不依赖界面数据结构。
using PlaybackError = std::variant<MediaOpenResult, MediaReadResult>;

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
    // 首次输出期限独立于每次读包，连续有包也不能延后它。
    void beginInitialOutputDeadline(std::chrono::steady_clock::time_point deadline) noexcept;
    // 最后一个活动轨道按时输出后撤销启动限制，不影响在途读包的原期限。
    bool finishInitialOutput() noexcept;
    bool isInitialOutputTimedOut() const noexcept;
    // 一次自动尝试的总期限独立于打开/探测/读取；首轮用户打开不设置它。
    void setRecoveryDeadline(std::chrono::steady_clock::time_point deadline) noexcept;
    bool finishRecoveryDeadline() noexcept;
    // 只有实际 PCM 写入或有效视频提交才刷新各自期限，收包不能代替输出。
    void beginOutputWatchdogs(bool audio, bool video, std::chrono::steady_clock::time_point deadline) noexcept;
    void noteAudioOutput(std::chrono::steady_clock::time_point deadline) noexcept;
    void noteVideoOutput(std::chrono::steady_clock::time_point deadline) noexcept;
    std::optional<MediaReadStage> liveDeadlineStage(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const noexcept;

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
    // 仅在打开/读取及 worker 已退出后进入，给协议关闭写入一次独立、不可延长的短期限。
    void beginCloseDeadline(std::chrono::milliseconds timeout) noexcept;
    // 成功后撤销操作期限，避免消费队列中的空闲时间算入下一次读取。
    void clearDeadline() noexcept;
    // 只读取原子标志和时钟；同一操作取消优先于期限。
    MediaIoStopReason stopReason() const noexcept;
    // FFmpeg 在阻塞 I/O 期间轮询，回调不等待、分配资源或调用 Qt。
    static int interruptCallback(void* opaque) noexcept;

    std::atomic<bool> cancelled_{false};
    std::atomic<Clock::duration::rep> deadline_{NoDeadline};
    std::atomic<Clock::duration::rep> initial_output_deadline_{NoDeadline};
    std::atomic<Clock::duration::rep> close_deadline_{NoDeadline};
    std::atomic<Clock::duration::rep> recovery_deadline_{NoDeadline};
    std::atomic<Clock::duration::rep> audio_output_deadline_{NoDeadline};
    std::atomic<Clock::duration::rep> video_output_deadline_{NoDeadline};
};
