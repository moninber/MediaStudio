#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

// 0 表示该维度不限；本地文件继续使用无硬限界的旧预读行为。
struct PacketQueueLimits {
    std::size_t packets = 0;
    std::uint64_t payload_bytes = 0;
    std::chrono::microseconds timestamp_span{0};
};

enum class PacketQueueLimit { None, Packets, PayloadBytes, TimestampSpan };
enum class PacketPushStatus { Queued, Full, PacketTooLarge, Aborted, Finished, Cancelled, InvalidPacket, AllocationFailed };
enum class PacketPushMode { Immediate, WaitForSpace };

struct PacketPushResult {
    PacketPushStatus status;
    PacketQueueLimit limit = PacketQueueLimit::None;
};

// 统计排队中的 AVPacket payload，不代表 FFmpeg/解码帧/设备或整个进程内存。
struct PacketQueueStats {
    std::size_t packets = 0;
    std::uint64_t payload_bytes = 0;
    std::optional<double> timestamp_span_seconds;
    std::size_t peak_packets = 0;
    std::uint64_t peak_payload_bytes = 0;
    std::uint64_t full_events = 0;
    std::uint64_t oversized_packets = 0;
    PacketQueueLimits limits;

    bool withinLimits() const noexcept
    {
        return (!limits.packets || packets <= limits.packets) &&
            (!limits.payload_bytes || payload_bytes <= limits.payload_bytes) &&
            (limits.timestamp_span.count() <= 0 || !timestamp_span_seconds ||
                *timestamp_span_seconds <= std::chrono::duration<double>(limits.timestamp_span).count());
    }
};
