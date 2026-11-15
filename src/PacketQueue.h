#pragma once

#include "PlaybackEpoch.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

extern "C" {
#include <libavcodec/avcodec.h>
}

class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // 建立独立的包引用并保存生产时的播放编号。
    bool push(const AVPacket* packet, PlaybackEpoch epoch);
    // 成功时一起交付包引用和编号，失败时保留两个输出参数的原值。
    bool pop(AVPacket* packet, PlaybackEpoch& epoch, bool block);
    // 唤醒等待中的消费者；切换请求时先等待旧消费者退出再重置。
    void abort();
    // 清空队列并重新接收入包；同请求 seek 后由消费者核对新旧编号。
    void reset();
    // 结束入队，让消费者继续取完队列中剩余的包。
    void finish();

    int size() const;
    bool isAborted() const;
    bool isFinished() const;

private:
    struct PacketDeleter {
        // 释放队列拥有的 FFmpeg 包结构及其引用。
        void operator()(AVPacket* packet) const noexcept;
    };

    struct TaggedPacket {
        std::unique_ptr<AVPacket, PacketDeleter> packet;
        PlaybackEpoch epoch;
    };

    // 在队列锁内丢弃全部待处理的包和编号。
    void clearLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<TaggedPacket> packets_;
    bool abort_requested_;
    bool finished_;
};
