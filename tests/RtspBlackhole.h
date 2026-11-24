#pragma once

#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

// 默认在回环地址提供一次 RTSP 会话；也可显式绑定私有 IPv4。
class RtspBlackhole {
public:
    enum class StallAt { ReadPacket, OpenInput, ProbeStreams, VideoReadPacket, RejectOpen, UnavailableEndpoint,
                         NotPublished, Unauthorized, TemporarilyBusy };
    enum class SetupTransport { None, Tcp, Udp };
    enum class StartupMedia { Audio, Video, AudioVideo };
    enum class KeyVideo { Immediate, AwaitRelease, NeverSend };
    enum class QueueBurst { Audio, Video, OversizedVideo };
    enum class VideoSignal { Normal, Late, Corrupt };

    // 默认先完成握手并提供有效音频，保留读包取消测试的行为。
    explicit RtspBlackhole(StallAt stall = StallAt::ReadPacket, const std::string& bind_address = "127.0.0.1",
                           SetupTransport transport = SetupTransport::Tcp);
    // 启播用例可发送大时间戳、双轨 RTP，并在非关键视频包后等待测试放行 IDR。
    RtspBlackhole(StartupMedia media, const std::string& bind_address, SetupTransport transport,
                  KeyVideo key_video = KeyVideo::Immediate, std::uint32_t video_offset_seconds = 0,
                  unsigned int max_sessions = 1, bool continuous = false,
                  std::chrono::milliseconds session_limit = std::chrono::seconds{14});
    // 在同一地址逐次提供预定阻塞阶段，验证暂停/继续确实重建连接。
    RtspBlackhole(std::vector<StallAt> stages, const std::string& bind_address, SetupTransport transport);
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
    // 计数由真实 accept 和连接析构更新，不用客户端状态冒充服务端断开。
    unsigned int acceptedConnections() const noexcept;
    unsigned int closedConnections() const noexcept;
    unsigned int playRequests() const noexcept;
    // UDP 媒体会话只有收到 TEARDOWN 才释放；控制连接 EOF 不能冒充远端会话删除。
    unsigned int teardownRequests() const noexcept;
    unsigned int activeRemoteSessions() const noexcept;
    // 服务端主动断开当前连接；连续流模式可单独停一轨，另一轨继续发包。
    void disconnectCurrent() noexcept;
    void setTrackStalled(bool audio, bool stalled) noexcept;
    // 暂停 RTP 与媒体时间，控制连接仍可处理 TEARDOWN；与丢包后 PTS 前进的单轨开关分开。
    void setMediaPaused(bool paused) noexcept;
    // 真正发送滞后时间戳或截短的 IDR 负载，用解码结果核验丢帧/损坏跳过分类。
    void setVideoSignal(VideoSignal signal) noexcept;
    unsigned int sentAudioPackets() const noexcept;
    unsigned int sentVideoPackets() const noexcept;
    // 连续正常供包至少一秒后按轨道瞬发；跨自动连接保留，用于验证 Full 共享有限预算。
    void releaseQueueBurst(QueueBurst burst) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
