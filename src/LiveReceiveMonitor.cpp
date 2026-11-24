#include "LiveReceiveMonitor.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace std::chrono_literals;
namespace {
double seconds(LiveReceiveMonitor::Clock::duration value) { return std::chrono::duration<double>(value).count(); }
void addSaturated(std::uint64_t& target, std::uint64_t amount)
{
    target += std::min(amount, std::numeric_limits<std::uint64_t>::max() - target);
}
}

// 新请求/自动连接均从空窗口、零累计和新的参考流开始，不承接上个连接的 DTS 锚点。
void LiveReceiveMonitor::reset(Clock::time_point started, LiveReferenceTrack reference)
{
    *this = {};
    started_ = started;
    active_ = reference != LiveReferenceTrack::None;
    reference_ = reference;
}

// 只撤销测量基准，保留真实接收累计与秒窗；不修改播放器代次、不触发重连。
void LiveReceiveMonitor::invalidate(LiveTimingState reason)
{
    ++domain_;
    anchor_.reset();
    latest_.reset();
    normal_step_.reset();
    timing_ = reason;
}

// 在入队前计接收量，包含等关键帧期间未入队的选中视频包；坏包计流量但不刷新有效包龄/推进。
void LiveReceiveMonitor::recordPacket(const LiveReceivedPacket& packet, Clock::time_point now)
{
    if (!active_ || !started_ || now < *started_ || (last_event_at_ && now < *last_event_at_) ||
        packet.track == LiveReferenceTrack::None || packet.payload_bytes == 0) return;
    last_event_at_ = now;
    const auto index = std::chrono::duration_cast<std::chrono::seconds>(now - *started_).count();
    auto& bucket = buckets_[static_cast<std::size_t>(index % HistorySize)];
    if (bucket.index != index) { bucket = {}; bucket.index = index; }
    addSaturated(packets_, 1); addSaturated(bytes_, packet.payload_bytes);
    addSaturated(bucket.packets, 1); addSaturated(bucket.bytes, packet.payload_bytes);
    if (!packet.corrupt) last_packet_at_ = now;
    if (packet.track != reference_) return; // 有视频就固定选视频，不用新鲜音频掩盖视频 DTS 失效。
    if (!packet.time_base_valid) { invalidate(LiveTimingState::InvalidTimeBase); return; }
    if (!packet.dts_seconds) { invalidate(LiveTimingState::MissingDts); return; }
    if (packet.corrupt || !std::isfinite(*packet.dts_seconds)) { invalidate(LiveTimingState::InvalidDts); return; }

    const double dts = *packet.dts_seconds;
    if (latest_) {
        const double step = dts - latest_->dts;
        // 已知正常间隔采用至少一秒/八倍间隔的跳变界线，未知节奏保留十秒容差。
        const double jump_limit = normal_step_ ? std::max(1.0, *normal_step_ * 8.0) : 10.0;
        if (step <= 0.0 || step > jump_limit) {
            invalidate(step == 0.0 ? LiveTimingState::RepeatedDts : step < 0.0
                ? LiveTimingState::BackwardDts : LiveTimingState::TimestampJump);
            return; // 异常包本身不成为新锚点，下一有效包开始两秒预热。
        }
        if (now - latest_->at > 3s) invalidate(LiveTimingState::Stale);
        else if (step <= 1.0) normal_step_ = step;
    }
    if (packet.duration_seconds && std::isfinite(*packet.duration_seconds) &&
        *packet.duration_seconds > 0.0 && *packet.duration_seconds <= 1.0) normal_step_ = packet.duration_seconds;
    const Point point{now, dts, domain_};
    if (!anchor_) { anchor_ = point; timing_ = LiveTimingState::WarmingUp; }
    latest_ = point;
    bucket.reference = point;
}

// 找边界之前最后一个同域参考点；不插值/合成缺失 DTS，不读当前未结束窗口的数据。
std::optional<LiveReceiveMonitor::Point> LiveReceiveMonitor::pointBefore(Clock::time_point boundary) const
{
    auto result = anchor_ && anchor_->at < boundary ? anchor_ : std::nullopt;
    for (const auto& bucket : buckets_) {
        const auto& point = bucket.reference;
        if (point && point->domain == domain_ && point->at < boundary && (!result || point->at > result->at)) result = point;
    }
    return result;
}

// 即使 FFmpeg 阻塞且没有新包，最近完整秒窗也能算出零；未满一秒仍为不可用而不是伪零。
LiveReceiveStats LiveReceiveMonitor::snapshot(Clock::time_point now) const
{
    if (!active_) return frozen_;
    LiveReceiveStats result;
    result.active = true;
    result.reference_track = reference_;
    result.timestamp_domain = domain_;
    result.received_packets = packets_;
    result.received_payload_bytes = bytes_;
    result.timing_state = timing_;
    if (!started_ || now < *started_) return result;
    if (last_packet_at_) result.last_packet_age_seconds = seconds(now - *last_packet_at_);
    if (latest_) result.reference_dts_age_seconds = seconds(now - latest_->at);
    if (anchor_) result.baseline_age_seconds = seconds(now - anchor_->at);
    if (latest_ && now - latest_->at > 3s) result.timing_state = LiveTimingState::Stale;
    const auto complete = std::chrono::duration_cast<std::chrono::seconds>(now - *started_).count();
    if (complete < 1) return result;
    const auto index = complete - 1;
    result.sample_sequence = static_cast<std::uint64_t>(complete);
    const auto& bucket = buckets_[static_cast<std::size_t>(index % HistorySize)];
    result.window_packets = bucket.index == index ? bucket.packets : 0;
    result.window_payload_bytes = bucket.index == index ? bucket.bytes : 0;
    result.application_bitrate_bps = static_cast<double>(*result.window_payload_bytes) * 8.0;
    const auto end = *started_ + std::chrono::seconds{complete};
    result.sample_age_seconds = seconds(now - end);
    if (!anchor_ || !latest_ || latest_->dts <= anchor_->dts || result.timing_state == LiveTimingState::Stale) return result;
    if (end - anchor_->at < 2s) { result.timing_state = LiveTimingState::WarmingUp; return result; }
    const auto left = pointBefore(end - 1s), right = pointBefore(end);
    if (!left || !right || end - right->at > 3s) return result;
    result.media_progress_rate = right->dts - left->dts; // 固定完整一秒的媒体秒 / 单调秒。
    result.relative_lag_seconds = seconds(end - anchor_->at) - (right->dts - anchor_->dts);
    result.timing_state = LiveTimingState::Ready;
    return result;
}

void LiveReceiveMonitor::finish(Clock::time_point now)
{
    if (!active_) return;
    frozen_ = snapshot(now);
    frozen_.active = false;
    active_ = false;
}
