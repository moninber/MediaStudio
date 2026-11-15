#pragma once

#include <memory>
#include <string>

// 只在回环地址提供一次 RTSP/TCP 会话；可在打开握手或成功送包后阻塞。
class RtspBlackhole {
public:
    enum class StallAt { ReadPacket, OpenInput };

    // 默认先完成握手并提供有效音频，保留读包取消测试的行为。
    explicit RtspBlackhole(StallAt stall = StallAt::ReadPacket);
    ~RtspBlackhole();
    RtspBlackhole(const RtspBlackhole&) = delete;
    RtspBlackhole& operator=(const RtspBlackhole&) = delete;

    // 访问系统分配的临时端口，不占用项目网络实验端口。
    std::string url() const;
    // 已进入指定阻塞点且 TCP 仍由夹具保持打开，不是连接失败或服务端 EOF。
    bool isHoldingConnection() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
