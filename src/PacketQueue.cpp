#include "PacketQueue.h"

#include <new>
#include <utility>

// 新队列可接收入包，消费者在队列为空时等待。
PacketQueue::PacketQueue()
    : abort_requested_(false)
    , finished_(false)
{
}

// 队列所有者先结束生产者和消费者，再析构并释放余下的包引用。
PacketQueue::~PacketQueue() = default;

// 由队列或入队中的临时所有者自动释放包引用。
void PacketQueue::PacketDeleter::operator()(AVPacket* packet) const noexcept
{
    av_packet_free(&packet);
}

// 入队复制 FFmpeg 引用，调用方仍可独立释放原包。
bool PacketQueue::push(const AVPacket* packet, PlaybackEpoch epoch)
{
    if (!packet) return false;

    std::unique_ptr<AVPacket, PacketDeleter> copy(av_packet_clone(packet));
    if (!copy) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (abort_requested_ || finished_) return false;

    try {
        packets_.push({std::move(copy), epoch});
    } catch (const std::bad_alloc&) {
        return false;
    }
    condition_.notify_one();
    return true;
}

// 出队时移动引用和对应编号，清空队列不会影响已经出队的包。
bool PacketQueue::pop(AVPacket* packet, PlaybackEpoch& epoch, bool block)
{
    if (!packet) return false;

    std::unique_lock<std::mutex> lock(mutex_);
    while (packets_.empty() && !abort_requested_ && !finished_) {
        if (!block) return false;
        condition_.wait(lock);
    }

    if (abort_requested_ || packets_.empty()) return false;

    auto front = std::move(packets_.front());
    packets_.pop();
    lock.unlock();

    av_packet_unref(packet);
    av_packet_move_ref(packet, front.packet.get());
    epoch = front.epoch;
    return true;
}

void PacketQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_requested_ = true;
        finished_ = true;
        clearLocked();
    }
    condition_.notify_all();
}

void PacketQueue::reset()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_requested_ = false;
        finished_ = false;
        clearLocked();
    }
    condition_.notify_all();
}

void PacketQueue::finish()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    condition_.notify_all();
}

int PacketQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(packets_.size());
}

bool PacketQueue::isAborted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return abort_requested_;
}

bool PacketQueue::isFinished() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
}

// 删除条目的同时释放 AVPacket，编号不会残留到下一次入队。
void PacketQueue::clearLocked()
{
    while (!packets_.empty()) {
        packets_.pop();
    }
}
