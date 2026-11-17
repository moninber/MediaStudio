#pragma once

#include <cstdint>
#include <memory>
#include <string>

// 默认在回环地址提供一次 RTSP 会话；也可显式绑定私有 IPv4。
class RtspBlackhole {
public:
    enum class StallAt { ReadPacket, OpenInput, ProbeStreams, VideoReadPacket, RejectOpen };
    enum class SetupTransport { None, Tcp, Udp };
    enum class StartupMedia { Audio, Video, AudioVideo };
    enum class KeyVideo { Immediate, AwaitRelease, NeverSend };

    // 默认先完成握手并提供有效音频，保留读包取消测试的行为。
    explicit RtspBlackhole(StallAt stall = StallAt::ReadPacket, const std::string& bind_address = "127.0.0.1",
                           SetupTransport transport = SetupTransport::Tcp);
    // 启播用例可发送大时间戳、双轨 RTP，并在非关键视频包后等待测试放行 IDR。
    RtspBlackhole(StartupMedia media, const std::string& bind_address, SetupTransport transport,
                  KeyVideo key_video = KeyVideo::Immediate, std::uint32_t video_offset_seconds = 0);
    ~RtspBlackhole();
    RtspBlackhole(const RtspBlackhole&) = delete;
    RtspBlackhole& operator=(const RtspBlackhole&) = delete;

    // 访问系统分配的临时端口，不占用项目网络实验端口。
    std::string url() const;
    // 已进入指定阻塞点且 TCP 仍由夹具保持打开，不是连接失败或服务端 EOF。
    bool isHoldingConnection() const noexcept;
    // 记录客户端实际发送的 SETUP 传输，而不是测试传入的期望值。
    SetupTransport setupTransport() const noexcept;
    // 放行被测试暂时扣留的关键视频包。
    void releaseKeyVideo() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
