#pragma once

#include "PlaybackEpoch.h"
#include "PacketQueueTypes.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <deque>

extern "C" {
#include <libavcodec/avcodec.h>
}

class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // 检查限界后才建立包引用。直播立即返回 Full；未来点播可等待空间，由 abort/reset/finish 唤醒。
    // WaitForSpace 不得在播放器快照/命令锁内调用；调用方在返回前保有原包。
    PacketPushResult push(const AVPacket* packet, PlaybackEpoch epoch,
                          PacketPushMode mode = PacketPushMode::Immediate);
    // 成功时一起交付包引用和编号，失败时保留两个输出参数的原值。
    bool pop(AVPacket* packet, PlaybackEpoch& epoch, bool block);
    // 唤醒等待中的消费者；切换请求时先等待旧消费者退出再重置。
    void abort();
    // 清空队列并重新接收入包；同请求 seek 后由消费者核对新旧编号。
    void reset();
    // 仅在旧生产者/消费者退出后配置新连接；time_base 使用打开时缓存的流值。
    void reset(PacketQueueLimits limits, AVRational time_base);
    // 结束入队，让消费者继续取完队列中剩余的包。
    void finish();

    int size() const;
    bool isAborted() const;
    bool isFinished() const;
    PacketQueueStats stats() const;

private:
    struct PacketDeleter {
        // 释放队列拥有的 FFmpeg 包结构及其引用。
        void operator()(AVPacket* packet) const noexcept;
    };

    struct TaggedPacket {
        std::unique_ptr<AVPacket, PacketDeleter> packet;
        PlaybackEpoch epoch;
        std::optional<std::int64_t> timestamp;
        std::uint64_t timeline;
    };

    struct TimestampCandidate {
        std::optional<std::int64_t> raw;
        std::optional<std::int64_t> trusted;
        bool uses_dts = false;
        std::uint64_t timeline;
    };

    TimestampCandidate timestampCandidateLocked(const AVPacket& packet, PlaybackEpoch epoch) const;
    PacketQueueLimit fullLimitLocked(std::uint64_t bytes, const TimestampCandidate& timestamp) const;
    std::optional<double> spanLocked(const TimestampCandidate* candidate = nullptr) const;
    void resetLocked();

    // 在队列锁内丢弃全部待处理的包和编号。
    void clearLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable space_changed_;
    std::deque<TaggedPacket> packets_;
    PacketQueueStats stats_;
    AVRational time_base_{0, 1};
    std::optional<std::int64_t> previous_timestamp_;
    std::optional<PlaybackEpoch> previous_epoch_;
    bool previous_uses_dts_ = false;
    std::uint64_t timeline_ = 0;
    std::uint64_t revision_ = 0; // reset/abort 的唤醒不能被随后一次 reset 抹掉。
    bool abort_requested_;
    bool finished_;
};
