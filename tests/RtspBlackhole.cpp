#include "RtspBlackhole.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
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

// 为 RTP/RTCP 绑定相邻 UDP 端口，仅监听与 RTSP 控制连接相同的地址。
class UdpStream {
public:
    explicit UdpStream(sockaddr_in address)
    {
        address.sin_port = 0;
        for (int retry = 0; retry < 64; ++retry) {
            auto rtp = std::make_unique<Socket>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
            auto rtcp = std::make_unique<Socket>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
            if (rtp->handle == INVALID_SOCKET || rtcp->handle == INVALID_SOCKET) socketError("UDP socket");
            int length = sizeof(address);
            if (bind(rtp->handle, reinterpret_cast<sockaddr*>(&address), length) == SOCKET_ERROR ||
                getsockname(rtp->handle, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
                socketError("UDP bind/getsockname");
            }
            port_ = ntohs(address.sin_port);
            address.sin_port = htons(static_cast<unsigned short>(port_ + 1));
            const bool bound = (port_ % 2 == 0) &&
                bind(rtcp->handle, reinterpret_cast<sockaddr*>(&address), length) != SOCKET_ERROR;
            address.sin_port = 0;
            if (bound) {
                rtp_ = std::move(rtp);
                rtcp_ = std::move(rtcp);
                return;
            }
        }
        throw std::runtime_error("could not reserve local RTP/RTCP ports");
    }

    // 报告 SETUP 的服务端端口对，发送实际 RTP 时剥去 TCP 交错帧头。
    std::string portPair() const { return std::to_string(port_) + '-' + std::to_string(port_ + 1); }
    bool sendRtp(sockaddr_in peer, unsigned short client_port, std::string_view packet) const
    {
        packet.remove_prefix(4);
        peer.sin_port = htons(client_port);
        return sendto(rtp_->handle, packet.data(), static_cast<int>(packet.size()), 0,
                      reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == static_cast<int>(packet.size());
    }

private:
    std::unique_ptr<Socket> rtp_;
    std::unique_ptr<Socket> rtcp_;
    unsigned short port_ = 0;
};

// 从 SETUP 请求中读取相邻的客户端 UDP 端口，不接受缺失或不完整的端口参数。
unsigned short readClientPort(std::string_view request)
{
    constexpr std::string_view prefix = "client_port=";
    const auto position = request.find(prefix);
    if (position == std::string_view::npos) return 0;
    request.remove_prefix(position + prefix.size());
    const auto end = request.data() + request.size();
    unsigned short first = 0;
    unsigned short second = 0;
    const auto rtp = std::from_chars(request.data(), end, first);
    if (rtp.ec != std::errc{} || rtp.ptr == end || *rtp.ptr != '-' || first == 0) return 0;
    const auto rtcp = std::from_chars(rtp.ptr + 1, end, second);
    return rtcp.ec == std::errc{} && second == first + 1 ? first : 0;
}

// 静态负载类型 0 是 PCMU；每包 160 个采样，RTP 时间戳按 20 ms 递增。
std::array<unsigned char, 176> pcmuRtp(std::uint16_t sequence, std::uint32_t origin = 0,
                                       unsigned char channel = 0)
{
    const std::uint32_t timestamp = origin + sequence * 160u;
    std::array<unsigned char, 176> bytes{};
    bytes.fill(0xff);
    bytes[0] = '$';
    bytes[1] = channel;
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
std::array<unsigned char, 28> h264Rtp(std::uint16_t sequence, std::uint32_t origin = 0)
{
    const std::uint32_t timestamp = origin + sequence * 3600u;  // 90 kHz 时钟，每帧 40 ms。
    return {
        '$', 0, 0, 24, 0x80, 0xe0,
        static_cast<unsigned char>(sequence >> 8), static_cast<unsigned char>(sequence),
        static_cast<unsigned char>(timestamp >> 24), static_cast<unsigned char>(timestamp >> 16),
        static_cast<unsigned char>(timestamp >> 8), static_cast<unsigned char>(timestamp),
        0x12, 0x34, 0x56, 0x78,
        0x65, 0x88, 0x84, 0x3a, 0x26, 0x28, 0x00, 0x08, 0xf6, 0x49, 0xd7, 0x5e
    };
}

// 与夹具的 SPS/PPS 同一次编码的非 IDR 帧，可使探测识别格式但不能满足关键包门控。
std::array<unsigned char, 21> h264NonIdr(std::uint16_t sequence, std::uint32_t origin)
{
    const auto scored = h264Rtp(sequence, origin);
    std::array<unsigned char, 21> packet{};
    std::copy_n(scored.begin(), 16, packet.begin());
    packet[3] = 17;
    constexpr std::array<unsigned char, 5> non_idr{0x41, 0x9a, 0x20, 0x26, 0x8b};
    std::copy(non_idr.begin(), non_idr.end(), packet.begin() + 16);
    return packet;
}

} // namespace

class RtspBlackhole::Impl {
public:
    // 先绑定再启动线程；若初始化失败，由成员析构关闭监听 socket。
    Impl(StallAt stall, const std::string& bind_address, SetupTransport transport,
         std::optional<StartupMedia> startup = {}, KeyVideo key_video = KeyVideo::Immediate,
         std::uint32_t video_offset_seconds = 0)
        : listener_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)), stall_(stall), bind_address_(bind_address),
          expected_transport_(transport), startup_(startup), key_video_(key_video), video_offset_seconds_(video_offset_seconds)
    {
        if (listener_.handle == INVALID_SOCKET) socketError("socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        if (InetPtonA(AF_INET, bind_address_.c_str(), &address.sin_addr) != 1) {
            throw std::runtime_error("RTSP fixture address must be a numeric IPv4 address");
        }
        // 只允许显式回环或私有地址，防止夹具监听所有接口或公开地址。
        const auto ip = ntohl(address.sin_addr.s_addr);
        if ((ip >> 24) != 127 && (ip >> 24) != 10 && (ip >> 20) != 0xAC1 && (ip >> 16) != 0xC0A8) {
            throw std::runtime_error("RTSP fixture bind address must be loopback or private IPv4");
        }
        int size = sizeof(address);
        if (bind(listener_.handle, reinterpret_cast<sockaddr*>(&address), size) == SOCKET_ERROR ||
            listen(listener_.handle, 1) == SOCKET_ERROR ||
            getsockname(listener_.handle, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR) {
            socketError("bind/listen/getsockname");
        }
        setNonBlocking(listener_.handle);
        url_ = "rtsp://" + bind_address_ + ":" + std::to_string(ntohs(address.sin_port)) + "/blackhole";
        listener_address_ = address;
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
    SetupTransport setupTransport() const noexcept { return setup_transport_.load(); }
    void releaseKeyVideo() noexcept { key_released_.store(true); }

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

    struct Track {
        std::unique_ptr<UdpStream> udp;
        unsigned short client_port = 0;
    };

    // 每条轨道使用独立的 UDP 端口对或 TCP 交错通道。
    template<std::size_t N>
    bool sendRtp(SOCKET socket, const sockaddr_in& peer, std::size_t track, const std::array<unsigned char, N>& packet)
    {
        const std::string_view bytes{reinterpret_cast<const char*>(packet.data()), packet.size()};
        auto& target = tracks_[track];
        return target.udp ? target.udp->sendRtp(peer, target.client_port, bytes) : writeAll(socket, bytes);
    }

    // 纯视频流量小于预读阈值；UDP 丢弃后续数据，TCP 则用半包等待剩余字节。
    bool sendVideoThenStall(SOCKET socket, const sockaddr_in& peer)
    {
        for (std::uint16_t seq = 1; seq <= 60; ++seq) {
            if (!sendRtp(socket, peer, 0, h264Rtp(seq))) return false;
        }
        if (tracks_[0].udp) return true;
        const auto incomplete = h264Rtp(61);
        return writeAll(socket, {reinterpret_cast<const char*>(incomplete.data()), 5});
    }

    // RTP-Info 保持零基准，而 RTP 样本远离零：FFmpeg 解复用后的实际媒体时间约 45 分钟。
    bool sendStartup(SOCKET socket, const sockaddr_in& peer, bool video, bool audio)
    {
        constexpr std::uint32_t origin_seconds = 2700;
        // 缺少首个 IDR 时探测仍可能读满分析时长；供应超过 5 秒的非关键视频时间线。
        constexpr std::uint16_t gate_at_frame = 150;
        const std::uint32_t video_origin = (origin_seconds + video_offset_seconds_) * 90000;
        const std::size_t audio_track = video ? 1u : 0u;
        // 直播加入点的音轨先于首个可解码视频 0.4 秒，避免把预探测的非关键视频当成主时钟起点。
        // 即时开始的测试两轨同时发送，只有视频的 RTP 时间戳可带人为偏移。
        const auto audio_origin = origin_seconds * 8000 + (video && key_video_ == KeyVideo::AwaitRelease ? 44800 : 0);
        const auto total = video ? gate_at_frame + 24 : 72;
        for (std::uint16_t seq = 1; seq <= total; ++seq) {
            if (video && seq > gate_at_frame && key_video_ == KeyVideo::NeverSend) {
                while (!stopping_.load() && Clock::now() < hard_deadline_) {
                    if (!sendRtp(socket, peer, 0, h264NonIdr(seq++, video_origin))) return false;
                    std::this_thread::sleep_for(40ms);
                }
                return false;
            }
            if (video && seq > gate_at_frame && key_video_ == KeyVideo::AwaitRelease) {
                while (!key_released_.load() && !stopping_.load() && Clock::now() < hard_deadline_) {
                    std::this_thread::sleep_for(5ms);
                }
                if (!key_released_.load()) return false;
            }
            if (audio) {
                const auto channel = static_cast<unsigned char>(audio_track * 2);
                const auto first = static_cast<std::uint16_t>(seq * 2 - 1);
                const auto second = static_cast<std::uint16_t>(seq * 2);
                if (!sendRtp(socket, peer, audio_track, pcmuRtp(first, audio_origin, channel)) ||
                    !sendRtp(socket, peer, audio_track, pcmuRtp(second, audio_origin, channel))) return false;
            }
            if (video) {
                const bool key = key_video_ == KeyVideo::Immediate || seq > gate_at_frame;
                const bool sent = key
                    ? sendRtp(socket, peer, 0, h264Rtp(seq, video_origin))
                    : sendRtp(socket, peer, 0, h264NonIdr(seq, video_origin));
                if (!sent) return false;
            }
        }
        return true;
    }

    // 完成最小 RTSP 握手并提供所选媒体，或收到首条请求后保持 TCP 连接但不回应。
    void serve()
    {
        if (!waitSocket(listener_.handle, true)) return;
        sockaddr_in peer_address{};
        int peer_length = sizeof(peer_address);
        const Socket peer(accept(listener_.handle, reinterpret_cast<sockaddr*>(&peer_address), &peer_length));
        if (peer.handle == INVALID_SOCKET) socketError("accept");
        setNonBlocking(peer.handle);
        const bool probing = stall_ == StallAt::ProbeStreams;
        const bool video = probing || stall_ == StallAt::VideoReadPacket ||
                           (startup_ && *startup_ != StartupMedia::Audio);
        const bool audio = !video || startup_ == StartupMedia::AudioVideo;
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
                if (stall_ == StallAt::RejectOpen) {
                    // 模拟错误诊断带回请求目标，验证播放器不会输出其中的查询参数。
                    const auto target = request.substr(9, request.find(' ', 9) - 9);
                    reply = "RTSP/1.0 403 " + target + request.substr(cseq, cseq_end - cseq) + "\r\n\r\n";
                    writeAll(peer.handle, reply);
                    return;
                }
                std::string sdp = "v=0\r\no=- 0 0 IN IP4 " + bind_address_ + "\r\n"
                    "s=MediaStudio CTest\r\nc=IN IP4 " + bind_address_ + "\r\nt=0 0\r\na=control:*\r\n";
                if (video) {
                    sdp += "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                           "a=fmtp:96 packetization-mode=1";
                    // 探测用例省略 SPS/PPS，其余握手正常，使流尺寸只能从后续 RTP 获得。
                    if (!probing) {
                        sdp += ";sprop-parameter-sets=Z0LACtolsBEAAAMAAQAAAwAyjxImoA==,aM4PyA==";
                    }
                    sdp += "\r\na=control:trackID=0\r\n";
                }
                if (audio) {
                    sdp += "m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000/1\r\na=control:trackID=";
                    sdp += video ? "1\r\n" : "0\r\n";
                }
                reply += "Content-Type: application/sdp\r\nContent-Base: " + url_ +
                         "/\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
            } else if (method == "SETUP") {
                const std::size_t track = request.find("/trackID=1 ") != std::string::npos ? 1 : 0;
                const std::string channels = track == 0 ? "interleaved=0-1" : "interleaved=2-3";
                const bool tcp = request.find("RTP/AVP/TCP") != std::string::npos &&
                                 request.find(channels) != std::string::npos;
                const unsigned short client_port = readClientPort(request);
                const bool udp = !tcp && client_port != 0 && request.find("RTP/AVP") != std::string::npos;
                const auto actual = tcp ? SetupTransport::Tcp : udp ? SetupTransport::Udp : SetupTransport::None;
                setup_transport_.store(actual);
                if (actual != expected_transport_ || actual == SetupTransport::None) {
                    throw std::runtime_error("unexpected RTP SETUP transport");
                }
                if (tcp) {
                    reply += "Transport: RTP/AVP/TCP;unicast;" + channels + "\r\n";
                } else {
                    tracks_[track].client_port = client_port;
                    tracks_[track].udp = std::make_unique<UdpStream>(listener_address_);
                    reply += "Transport: RTP/AVP/UDP;unicast;client_port=" + std::to_string(client_port) + '-' +
                        std::to_string(client_port + 1) + ";server_port=" + tracks_[track].udp->portPair() + "\r\n";
                }
                reply += "Session: 1234abcd;timeout=60\r\n\r\n";
            } else if (method == "PLAY") {
                reply += "Session: 1234abcd\r\nRTP-Info: url=" + url_ +
                         "/trackID=0;seq=1;rtptime=" + (video ? "3600" : "160");
                if (video && audio) reply += ",url=" + url_ + "/trackID=1;seq=1;rtptime=160";
                reply += "\r\n\r\n";
            } else if (method == "TEARDOWN" || method == "GET_PARAMETER") {
                reply += "Session: 1234abcd\r\n\r\n";
            } else {
                throw std::runtime_error("unexpected RTSP method: " + method);
            }
            if (!writeAll(peer.handle, reply) || method == "TEARDOWN") return;
            if (method == "PLAY") {
                if (startup_) {
                    holding_.store(true);
                    if (!sendStartup(peer.handle, peer_address, video, audio)) return;
                } else if (probing) {
                    // PLAY 成功，但探测一直等缺失的解码参数。对端关闭前保留 TCP。
                    holding_.store(true);
                    continue;
                } else if (video) {
                    if (!sendVideoThenStall(peer.handle, peer_address)) return;
                } else {
                    for (std::uint16_t seq = 1; seq <= 96; ++seq) {
                        if (!sendRtp(peer.handle, peer_address, 0, pcmuRtp(seq))) return;
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
    const std::string bind_address_;
    const SetupTransport expected_transport_;
    const std::optional<StartupMedia> startup_;
    const KeyVideo key_video_;
    const std::uint32_t video_offset_seconds_;
    std::atomic<bool> key_released_{false};
    sockaddr_in listener_address_{};
    std::array<Track, 2> tracks_;
    std::string url_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> holding_{false};
    std::atomic<SetupTransport> setup_transport_{SetupTransport::None};
    const Clock::time_point hard_deadline_ = Clock::now() + 14s;
    std::thread worker_;
};

// 创建指定地址上的单会话夹具，地址默认仍是回环。
RtspBlackhole::RtspBlackhole(StallAt stall, const std::string& bind_address, SetupTransport transport)
    : impl_(std::make_unique<Impl>(stall, bind_address, transport)) {}
RtspBlackhole::RtspBlackhole(StartupMedia media, const std::string& bind_address, SetupTransport transport,
                           KeyVideo key_video, std::uint32_t video_offset_seconds)
    : impl_(std::make_unique<Impl>(StallAt::ReadPacket, bind_address, transport, media, key_video, video_offset_seconds)) {}
RtspBlackhole::~RtspBlackhole() = default;
std::string RtspBlackhole::url() const { return impl_->url(); }
bool RtspBlackhole::isHoldingConnection() const noexcept { return impl_->isHoldingConnection(); }
RtspBlackhole::SetupTransport RtspBlackhole::setupTransport() const noexcept { return impl_->setupTransport(); }
void RtspBlackhole::releaseKeyVideo() noexcept { impl_->releaseKeyVideo(); }
