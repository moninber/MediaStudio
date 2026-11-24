#include "PacketQueue.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace {
// 与直播时钟的跳变容差一致；跨时间域不能计算队列跨度。
constexpr long double TimestampJumpSeconds = 10.0L;
}

PacketQueue::PacketQueue() : abort_requested_(false), finished_(false) {}

// 所有者先取消并接回生产者/消费者，再析构；条目的独占所有权释放剩余引用。
PacketQueue::~PacketQueue() = default;

void PacketQueue::PacketDeleter::operator()(AVPacket* packet) const noexcept
{
    av_packet_free(&packet);
}

// 优先 DTS，缺少时回退 PTS。缺失、倒退、换代、DTS/PTS 切换或大跳变都重建测量基准。
// 这里只计算候选；Full、取消和分配失败不能改变已入队数据的统计域。
PacketQueue::TimestampCandidate PacketQueue::timestampCandidateLocked(const AVPacket& packet, PlaybackEpoch epoch) const
{
    TimestampCandidate result{{}, {}, packet.dts != AV_NOPTS_VALUE, timeline_};
    if (time_base_.num > 0 && time_base_.den > 0) {
        const auto raw = result.uses_dts ? packet.dts : packet.pts;
        if (raw != AV_NOPTS_VALUE) result.raw = result.trusted = raw;
    }
    const bool changed = previous_epoch_ && *previous_epoch_ != epoch;
    bool discontinuity = !result.raw || changed || (previous_timestamp_ && result.uses_dts != previous_uses_dts_);
    if (result.raw && previous_timestamp_ && !changed && result.uses_dts == previous_uses_dts_) {
        // 先转长浮点再相减，避免极值 PTS 的有符号溢出；不将负 DTS/大绝对 PTS 当成坏数据。
        const long double seconds = (static_cast<long double>(*result.raw) - *previous_timestamp_) * time_base_.num / time_base_.den;
        if (seconds < 0.0L || seconds > TimestampJumpSeconds) {
            discontinuity = true;
            result.trusted.reset(); // 跳变包本身不可用于显示跨度，后续包从新基准开始。
        }
    }
    if (discontinuity) ++result.timeline;
    return result;
}

// 只有队头和队尾属于同一连续可信时间域，才能报告排队包的首尾时间差。
std::optional<double> PacketQueue::spanLocked(const TimestampCandidate* candidate) const
{
    if (packets_.empty()) return candidate && candidate->trusted ? std::optional<double>{0.0} : std::nullopt;
    const auto& first = packets_.front();
    const auto last = candidate ? candidate->trusted : packets_.back().timestamp;
    const auto timeline = candidate ? candidate->timeline : packets_.back().timeline;
    if (!first.timestamp || !last || first.timeline != timeline || time_base_.num <= 0 || time_base_.den <= 0) return {};
    const long double seconds = (static_cast<long double>(*last) - *first.timestamp) * time_base_.num / time_base_.den;
    if (!std::isfinite(seconds) || seconds < 0.0L) return {};
    return static_cast<double>(seconds);
}

PacketQueueLimit PacketQueue::fullLimitLocked(std::uint64_t bytes, const TimestampCandidate& timestamp) const
{
    const auto& limits = stats_.limits;
    if (limits.packets && packets_.size() >= limits.packets) return PacketQueueLimit::Packets;
    if (limits.payload_bytes && bytes > limits.payload_bytes - stats_.payload_bytes) return PacketQueueLimit::PayloadBytes;
    if (limits.timestamp_span.count() > 0) {
        if (const auto span = spanLocked(&timestamp)) {
            if (*span > std::chrono::duration<double>(limits.timestamp_span).count() + 1e-9) return PacketQueueLimit::TimestampSpan;
        }
    }
    return PacketQueueLimit::None;
}

// 检查、等待、克隆引用和更新统计在同一队列锁内完成；不存在先检查容量再超额入队的竞争。
PacketPushResult PacketQueue::push(const AVPacket* packet, PlaybackEpoch epoch, PacketPushMode mode)
{
    if (!packet || packet->size < 0 || (packet->size > 0 && !packet->data)) return {PacketPushStatus::InvalidPacket};
    const auto bytes = static_cast<std::uint64_t>(packet->size);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto revision = revision_;
    TimestampCandidate timestamp;
    for (;;) {
        if (abort_requested_) return {PacketPushStatus::Aborted};
        if (finished_) return {PacketPushStatus::Finished};
        if (revision != revision_) return {PacketPushStatus::Cancelled};
        if (stats_.limits.payload_bytes && bytes > stats_.limits.payload_bytes) {
            ++stats_.oversized_packets;
            return {PacketPushStatus::PacketTooLarge, PacketQueueLimit::PayloadBytes};
        }
        timestamp = timestampCandidateLocked(*packet, epoch);
        const auto limit = fullLimitLocked(bytes, timestamp);
        if (limit == PacketQueueLimit::None) break;
        if (mode == PacketPushMode::Immediate) {
            ++stats_.full_events;
            return {PacketPushStatus::Full, limit};
        }
        // wait 原子释放队列锁，pop 或生命周期命令唤醒后重新核对全部条件。
        // revision 覆盖 abort 紧接 reset 的竞争，旧生产者不能混入新连接。
        space_changed_.wait(lock);
    }
    if (bytes > std::numeric_limits<std::uint64_t>::max() - stats_.payload_bytes) return {PacketPushStatus::AllocationFailed};
    std::unique_ptr<AVPacket, PacketDeleter> copy(av_packet_clone(packet));
    if (!copy) return {PacketPushStatus::AllocationFailed};
    try {
        packets_.push_back({std::move(copy), epoch, timestamp.trusted, timestamp.timeline});
    } catch (const std::bad_alloc&) {
        return {PacketPushStatus::AllocationFailed};
    }
    timeline_ = timestamp.timeline;
    previous_timestamp_ = timestamp.raw;
    previous_epoch_ = epoch;
    previous_uses_dts_ = timestamp.uses_dts;
    stats_.packets = packets_.size();
    stats_.payload_bytes += bytes;
    stats_.peak_packets = std::max(stats_.peak_packets, stats_.packets);
    stats_.peak_payload_bytes = std::max(stats_.peak_payload_bytes, stats_.payload_bytes);
    lock.unlock();
    condition_.notify_one();
    return {PacketPushStatus::Queued};
}

// 出队扣减占用并唤醒生产者，随后在锁外移动包引用；已出队的包不受 abort/reset 影响。
bool PacketQueue::pop(AVPacket* packet, PlaybackEpoch& epoch, bool block)
{
    if (!packet) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto revision = revision_;
    while (packets_.empty() && !abort_requested_ && !finished_ && revision == revision_) {
        if (!block) return false;
        condition_.wait(lock);
    }
    if (abort_requested_ || revision != revision_ || packets_.empty()) return false;
    auto front = std::move(packets_.front());
    packets_.pop_front();
    stats_.packets = packets_.size();
    stats_.payload_bytes -= static_cast<std::uint64_t>(front.packet->size);
    lock.unlock();
    space_changed_.notify_all();
    av_packet_unref(packet);
    av_packet_move_ref(packet, front.packet.get());
    epoch = front.epoch;
    return true;
}

void PacketQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++revision_;
        abort_requested_ = finished_ = true;
        clearLocked();
    }
    condition_.notify_all();
    space_changed_.notify_all();
}

void PacketQueue::resetLocked()
{
    ++revision_;
    abort_requested_ = finished_ = false;
    clearLocked();
    const auto limits = stats_.limits;
    stats_ = {};
    stats_.limits = limits;
    previous_timestamp_.reset();
    previous_epoch_.reset();
    previous_uses_dts_ = false;
    timeline_ = 0;
}

// 同连接刷新保留限额/time_base；新连接用重载显式替换，防止直播限制流入本地文件。
void PacketQueue::reset()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resetLocked();
    }
    condition_.notify_all();
    space_changed_.notify_all();
}

void PacketQueue::reset(PacketQueueLimits limits, AVRational time_base)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.limits = limits;
        time_base_ = time_base;
        resetLocked();
    }
    condition_.notify_all();
    space_changed_.notify_all();
}

void PacketQueue::finish()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    condition_.notify_all();
    space_changed_.notify_all();
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

PacketQueueStats PacketQueue::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = stats_;
    result.timestamp_span_seconds = spanLocked();
    return result;
}

// clear 在队列锁内释放每个条目引用；保留峰值和故障计数供断流后的历史观察。
void PacketQueue::clearLocked()
{
    packets_.clear();
    stats_.packets = 0;
    stats_.payload_bytes = 0;
}
