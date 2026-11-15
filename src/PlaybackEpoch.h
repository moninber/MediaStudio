#pragma once

#include <cstdint>

// 同时标识播放请求、该请求内的连接和当前输出代次。
struct PlaybackEpoch {
    std::uint64_t request_id{};
    std::uint64_t connection_attempt_id{};
    std::uint64_t generation{};
};

// 只有三个编号都匹配，数据才属于同一次输出。
constexpr bool operator==(const PlaybackEpoch& left, const PlaybackEpoch& right) noexcept
{
    return left.request_id == right.request_id &&
           left.connection_attempt_id == right.connection_attempt_id &&
           left.generation == right.generation;
}

// 任一编号变化后，调用方可以用此比较识别旧包或旧帧。
constexpr bool operator!=(const PlaybackEpoch& left, const PlaybackEpoch& right) noexcept
{
    return !(left == right);
}
