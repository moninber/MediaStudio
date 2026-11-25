#pragma once

#include "PlaybackSource.h"
#include <chrono>
#include <memory>
#include <string>

// 通过 FFmpeg 自己的 RTMP/SRT listener 输出供流，仅用于测试；产品始终为拉流客户端。
// 提供真实 H.264/AAC、可冻结媒体和连续接入，不实现第二套 RTMP/SRT 协议栈。
class LiveProtocolServer {
public:
    explicit LiveProtocolServer(PlaybackProtocol protocol, unsigned int connections = 6,
        std::chrono::seconds lifetime = std::chrono::seconds{45}, unsigned int video_frame_rate = 25,
        std::chrono::milliseconds video_start_delay = std::chrono::milliseconds{0});
    ~LiveProtocolServer();
    LiveProtocolServer(const LiveProtocolServer&) = delete;
    LiveProtocolServer& operator=(const LiveProtocolServer&) = delete;
    std::string url() const;
    void setMediaPaused(bool paused) noexcept;
    void disconnectCurrent() noexcept;
    unsigned int acceptedConnections() const noexcept;
    unsigned int activeConnections() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
