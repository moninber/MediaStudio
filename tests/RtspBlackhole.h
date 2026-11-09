#pragma once

#include <memory>
#include <string>

// 只在回环地址提供一次 RTSP/TCP 会话。发送有效 RTP 后保持连接、停止送包。
class RtspBlackhole {
public:
    RtspBlackhole();
    ~RtspBlackhole();
    RtspBlackhole(const RtspBlackhole&) = delete;
    RtspBlackhole& operator=(const RtspBlackhole&) = delete;

    // 访问系统分配的临时端口，不占用项目网络实验端口。
    std::string url() const;
    // TCP 仍由夹具保持打开，停止发包并非服务端 EOF。
    bool isHoldingConnection() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
