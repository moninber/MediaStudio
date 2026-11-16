#include "RtspBlackhole.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// 管理 Winsock 初始化和每个 socket 的独立归属。
struct Winsock {
    Winsock()
    {
        WSADATA data{};
        const int code = WSAStartup(MAKEWORD(2, 2), &data);
        if (code != 0) {
            throw std::runtime_error("WSAStartup: " + std::to_string(code));
        }
    }
    ~Winsock() { WSACleanup(); }
};

struct Socket {
    explicit Socket(SOCKET socket) : handle(socket) {}
    ~Socket() { if (handle != INVALID_SOCKET) closesocket(handle); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SOCKET handle;
};

// 统一报告 socket 错误，方便区分测试夹具失败与 FFmpeg 返回状态错误。
[[noreturn]] void socketError(const char* operation)
{
    throw std::runtime_error(std::string(operation) + ": " + std::to_string(WSAGetLastError()));
}

// 用非阻塞 socket 加短轮询，使连接、收发和析构 join 都有界。
void setNonBlocking(SOCKET socket)
{
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) == SOCKET_ERROR) {
        socketError("ioctlsocket");
    }
}

// 静态负载类型 0 是 PCMU；每包 160 个采样，RTP 时间戳按 20 ms 递增。
std::array<unsigned char, 176> pcmuRtp(std::uint16_t sequence)
{
    const std::uint32_t timestamp = sequence * 160u;
    std::array<unsigned char, 176> bytes{};
    bytes.fill(0xff);
    bytes[0] = '$';
    bytes[1] = 0;
    bytes[2] = 0;
    bytes[3] = 172;
    bytes[4] = 0x80;
    bytes[5] = 0;
    bytes[6] = static_cast<unsigned char>(sequence >> 8);
    bytes[7] = static_cast<unsigned char>(sequence);
    for (unsigned i = 0; i < 4; ++i) {
        bytes[8 + i] = static_cast<unsigned char>(timestamp >> (8 * (3 - i)));
    }
    // bytes[12..15] 为恒定 SSRC，后续 0xff 为有效的 PCMU 静音样本。
    return bytes;
}

// 32x32 白色 YUV420P 的 H.264 Baseline IDR，SPS/PPS 由 SDP 提供，无需运行编码器。
std::array<unsigned char, 28> h264Rtp(std::uint16_t sequence)
{
    const std::uint32_t timestamp = sequence * 3600u;  // 90 kHz 时钟，每帧 40 ms。
    return {
        '$', 0, 0, 24, 0x80, 0xe0,
        static_cast<unsigned char>(sequence >> 8), static_cast<unsigned char>(sequence),
        static_cast<unsigned char>(timestamp >> 24), static_cast<unsigned char>(timestamp >> 16),
        static_cast<unsigned char>(timestamp >> 8), static_cast<unsigned char>(timestamp),
        0x12, 0x34, 0x56, 0x78,
        0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x08, 0xf6, 0x49, 0xd7, 0x5e
    };
}

} // namespace

class RtspBlackhole::Impl {
public:
    // 先绑定再启动线程；若初始化失败，由成员析构关闭监听 socket。
    explicit Impl(StallAt stall) : listener_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)), stall_(stall)
    {
        if (listener_.handle == INVALID_SOCKET) socketError("socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(address);
        if (bind(listener_.handle, reinterpret_cast<sockaddr*>(&address), size) == SOCKET_ERROR ||
            listen(listener_.handle, 1) == SOCKET_ERROR ||
            getsockname(listener_.handle, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR) {
            socketError("bind/listen/getsockname");
        }
        setNonBlocking(listener_.handle);
        url_ = "rtsp://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/blackhole";
        worker_ = std::thread([this] {
            try {
                serve();
            } catch (const std::exception& error) {
                std::cerr << "RTSP fixture: " << error.what() << '\n';
            }
            holding_.store(false);
        });
    }

    // 先通知服务线程并等待其释放连接，再释放监听 socket 和 Winsock。
    ~Impl()
    {
        stopping_.store(true);
        worker_.join();
    }

    std::string url() const { return url_; }
    bool isHoldingConnection() const noexcept { return holding_.load(); }

private:
    // 夹具自身的期限是故障保护；正常测试在此之前由 FFmpeg 返回。
    bool waitSocket(SOCKET socket, bool read)
    {
        while (!stopping_.load() && Clock::now() < hard_deadline_) {
            fd_set sockets;
            FD_ZERO(&sockets);
            FD_SET(socket, &sockets);
            timeval timeout{0, 50000};
            const int ready = select(0, read ? &sockets : nullptr,
                                     read ? nullptr : &sockets, nullptr, &timeout);
            if (ready == SOCKET_ERROR) socketError("select");
            if (ready > 0) return true;
        }
        return false;
    }

    // TCP 可能只接受部分数据，继续发送余下字节直至完成或到期。
    bool writeAll(SOCKET socket, std::string_view remaining)
    {
        while (!remaining.empty() && waitSocket(socket, false)) {
            const int written = send(socket, remaining.data(), static_cast<int>(remaining.size()), 0);
            if (written == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
                return false;
            }
            if (written == 0) return false;
            remaining.remove_prefix(static_cast<std::size_t>(written));
        }
        return remaining.empty();
    }

    // 从同一 TCP 流读取 RTSP 文本，跳过客户端可能发来的 interleaved RTCP。
    std::string readRequest(SOCKET socket, std::string& pending)
    {
        for (;;) {
            if (!pending.empty() && pending.front() == '$') {
                if (pending.size() >= 4) {
                    const std::size_t size = 4u + 256u * static_cast<unsigned char>(pending[2]) +
                                             static_cast<unsigned char>(pending[3]);
                    if (pending.size() >= size) {
                        pending.erase(0, size);
                        continue;
                    }
                }
            } else if (const auto end = pending.find("\r\n\r\n"); end != std::string::npos) {
                std::string result = pending.substr(0, end + 4);
                pending.erase(0, end + 4);
                return result;
            }
            if (pending.size() > 8192 || !waitSocket(socket, true)) return {};
            char buffer[1024];
            const int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (received <= 0) return {};
            pending.append(buffer, received);
        }
    }

    // 纯视频流量小于控制器预读阈值，随后发送半包，使下一次读取等待剩余字节。
    bool sendVideoThenStall(SOCKET socket)
    {
        for (std::uint16_t seq = 1; seq <= 60; ++seq) {
            const auto packet = h264Rtp(seq);
            if (!writeAll(socket, {reinterpret_cast<const char*>(packet.data()), packet.size()})) return false;
        }
        const auto incomplete = h264Rtp(61);
        return writeAll(socket, {reinterpret_cast<const char*>(incomplete.data()), 5});
    }

    // 完成最小 RTSP 握手并提供所选媒体，或收到首条请求后保持 TCP 连接但不回应。
    void serve()
    {
        if (!waitSocket(listener_.handle, true)) return;
        const Socket peer(accept(listener_.handle, nullptr, nullptr));
        if (peer.handle == INVALID_SOCKET) socketError("accept");
        setNonBlocking(peer.handle);
        const bool video = stall_ == StallAt::VideoReadPacket;
        std::string pending;
        while (!stopping_.load()) {
            const auto request = readRequest(peer.handle, pending);
            if (request.empty()) return;
            if (stall_ == StallAt::OpenInput) {
                holding_.store(true);
                // 继续读 socket 以识别客户端主动关闭；服务端始终不回 RTSP 响应。
                while (!readRequest(peer.handle, pending).empty()) {}
                return;
            }
            const auto cseq = request.find("\r\nCSeq:");
            if (cseq == std::string::npos) throw std::runtime_error("missing CSeq");
            const auto cseq_end = request.find("\r\n", cseq + 2);
            const auto method = request.substr(0, request.find(' '));
            std::string reply = "RTSP/1.0 200 OK" + request.substr(cseq, cseq_end - cseq) + "\r\n";
            if (method == "OPTIONS") {
                reply += "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n";
            } else if (method == "DESCRIBE") {
                std::string sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\n"
                    "s=MediaStudio CTest\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\na=control:*\r\n";
                if (video) {
                    sdp += "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                           "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0LACtolsBEAAAMAAQAAAwAyjxImoA==,aM4PyA==\r\n";
                } else {
                    sdp += "m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000/1\r\n";
                }
                sdp += "a=control:trackID=0\r\n";
                reply += "Content-Type: application/sdp\r\nContent-Base: " + url_ +
                         "/\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
            } else if (method == "SETUP") {
                if (request.find("RTP/AVP/TCP") == std::string::npos ||
                    request.find("interleaved=0-1") == std::string::npos) {
                    throw std::runtime_error("client did not request interleaved RTP/TCP");
                }
                reply += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                         "Session: 1234abcd;timeout=60\r\n\r\n";
            } else if (method == "PLAY") {
                reply += "Session: 1234abcd\r\nRTP-Info: url=" + url_ +
                         "/trackID=0;seq=1;rtptime=" + (video ? "3600" : "160") + "\r\n\r\n";
            } else if (method == "TEARDOWN" || method == "GET_PARAMETER") {
                reply += "Session: 1234abcd\r\n\r\n";
            } else {
                throw std::runtime_error("unexpected RTSP method: " + method);
            }
            if (!writeAll(peer.handle, reply) || method == "TEARDOWN") return;
            if (method == "PLAY") {
                if (video) {
                    if (!sendVideoThenStall(peer.handle)) return;
                } else {
                    for (std::uint16_t seq = 1; seq <= 96; ++seq) {
                        const auto packet = pcmuRtp(seq);
                        if (!writeAll(peer.handle, {reinterpret_cast<const char*>(packet.data()), packet.size()})) return;
                    }
                }
                // 不送更多 RTP，也不关 socket。read 必须靠客户端中断或期限结束。
                holding_.store(true);
            }
        }
    }

    Winsock winsock_;
    Socket listener_;
    const StallAt stall_;
    std::string url_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> holding_{false};
    const Clock::time_point hard_deadline_ = Clock::now() + 14s;
    std::thread worker_;
};

RtspBlackhole::RtspBlackhole(StallAt stall) : impl_(std::make_unique<Impl>(stall)) {}
RtspBlackhole::~RtspBlackhole() = default;
std::string RtspBlackhole::url() const { return impl_->url(); }
bool RtspBlackhole::isHoldingConnection() const noexcept { return impl_->isHoldingConnection(); }
