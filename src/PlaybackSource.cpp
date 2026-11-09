#include "PlaybackSource.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace {

// URL 协议名只含 ASCII；转换不依赖操作系统语言设置。
char asciiLower(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

// 识别已建模的 URL 协议，未列出的地址不会被当作直播或点播。
PlaybackProtocol protocolOf(std::string_view address) noexcept
{
    const auto delimiter = address.find("://");
    if (delimiter == std::string_view::npos) {
        return PlaybackProtocol::Unknown;
    }
    const auto scheme = address.substr(0, delimiter);
    const auto is = [scheme](std::string_view expected) {
        return scheme.size() == expected.size() &&
               std::equal(scheme.begin(), scheme.end(), expected.begin(),
                          [](char actual, char wanted) { return asciiLower(actual) == wanted; });
    };
    if (is("rtsp")) return PlaybackProtocol::Rtsp;
    if (is("rtmp")) return PlaybackProtocol::Rtmp;
    if (is("srt")) return PlaybackProtocol::Srt;
    if (is("http")) return PlaybackProtocol::Http;
    if (is("https")) return PlaybackProtocol::Https;
    return PlaybackProtocol::Unknown;
}

// 网络显示文本不使用主机、路径或查询参数，避免显示鉴权信息。
const char* protocolDisplayName(PlaybackProtocol protocol) noexcept
{
    switch (protocol) {
        case PlaybackProtocol::Rtsp: return "RTSP";
        case PlaybackProtocol::Rtmp: return "RTMP";
        case PlaybackProtocol::Srt: return "SRT";
        case PlaybackProtocol::Http: return "HTTP";
        case PlaybackProtocol::Https: return "HTTPS";
        default: return "Network source";
    }
}

// Windows 盘符的冒号属于路径；其他路径前缀中的冒号可能让 FFmpeg 选中协议。
bool isLocalPath(std::string_view path) noexcept
{
    const auto colon = path.find(':');
    if (colon == 1 && asciiLower(path[0]) >= 'a' && asciiLower(path[0]) <= 'z') {
        return true;
    }
    return colon == std::string_view::npos || colon > path.find_first_of("/\\");
}

} // namespace

// 保存工厂已经选定的源类型、地址和显示数据。
PlaybackSource::PlaybackSource(PlaybackSourceKind kind, std::string address,
                               std::string display_name, LiveSourceOptions options)
    : kind_(kind), address_(std::move(address)), display_name_(std::move(display_name)),
      live_options_(options)
{
}

// 提取跨斜杠形式的文件名，但原始路径不做编码或分隔符转换。
PlaybackSource PlaybackSource::localFile(std::string path, std::string display_name)
{
    if (display_name.empty()) {
        const auto separator = path.find_last_of("/\\");
        display_name = separator == std::string::npos ? path : path.substr(separator + 1);
    }
    return PlaybackSource(PlaybackSourceKind::LocalFile, std::move(path), std::move(display_name));
}

// 从协议生成固定显示名，保留带有调用参数的原始地址供打开操作使用。
PlaybackSource PlaybackSource::networkLive(std::string url, LiveSourceOptions options)
{
    const auto display_name = protocolDisplayName(protocolOf(url));
    return PlaybackSource(PlaybackSourceKind::NetworkLive, std::move(url), display_name, options);
}

// 点播不携带 RTSP 传输或直播预设。
PlaybackSource PlaybackSource::networkVod(std::string url)
{
    const auto display_name = protocolDisplayName(protocolOf(url));
    return PlaybackSource(PlaybackSourceKind::NetworkVod, std::move(url), display_name);
}

// 分源类型取协议，本地路径不参加网络 URL 解析。
PlaybackProtocol PlaybackSource::protocol() const noexcept
{
    return kind_ == PlaybackSourceKind::LocalFile ? PlaybackProtocol::File : protocolOf(address_);
}

// 类型不匹配时没有直播选项。
const LiveSourceOptions* PlaybackSource::liveOptions() const noexcept
{
    return kind_ == PlaybackSourceKind::NetworkLive ? &live_options_ : nullptr;
}

// 只做结构检查；认证、主机解析与传输可用性仍由实际连接判断。
bool PlaybackSource::isValid() const noexcept
{
    if (address_.empty() || address_.find('\0') != std::string::npos) {
        return false;
    }
    if (kind_ == PlaybackSourceKind::LocalFile) {
        return isLocalPath(address_);
    }

    const auto transport = protocol();
    const bool matches_kind = kind_ == PlaybackSourceKind::NetworkLive
        ? transport == PlaybackProtocol::Rtsp || transport == PlaybackProtocol::Rtmp ||
          transport == PlaybackProtocol::Srt
        : transport == PlaybackProtocol::Http || transport == PlaybackProtocol::Https;
    if (!matches_kind || std::any_of(address_.begin(), address_.end(), [](unsigned char value) {
            return value <= ' ' || value == 127;
        })) {
        return false;
    }
    const auto rest = std::string_view(address_).substr(address_.find("://") + 3);
    const auto authority = rest.substr(0, rest.find_first_of("/?#"));
    const auto credentials = authority.rfind('@');
    // 点播允许查询参数签名，暂不接受 URL userinfo；直播保留认证地址形式。
    return !authority.empty() && (credentials == std::string_view::npos ||
        (kind_ == PlaybackSourceKind::NetworkLive && credentials + 1 < authority.size()));
}

// 网络源共享期限设施，业务策略按三类源独立选择。
PlaybackPolicy playbackPolicy(const PlaybackSource& source) noexcept
{
    using namespace std::chrono_literals;
    PlaybackPolicy policy;
    switch (source.kind()) {
        case PlaybackSourceKind::LocalFile:
            break;
        case PlaybackSourceKind::NetworkLive:
            policy.pause = PauseBehavior::DisconnectLive;
            policy.end_of_stream = EndOfStreamBehavior::RecoverLive;
            policy.queue_full = QueueFullBehavior::ReconnectLive;
            policy.reconnect_position = ReconnectPosition::LiveEdge;
            policy.open_timeout = 5s;
            policy.probe_timeout = 5s;
            policy.read_timeout = source.liveOptions()->preset == LivePreset::LowLatency ? 3s : 5s;
            break;
        case PlaybackSourceKind::NetworkVod:
            policy.pause = PauseBehavior::FreezeVod;
            policy.queue_full = QueueFullBehavior::Backpressure;
            policy.reconnect_position = ReconnectPosition::LastPlayedPosition;
            policy.open_timeout = 10s;
            policy.probe_timeout = 10s;
            policy.read_timeout = 10s;
            break;
    }
    return policy;
}

// 点播时长、定位、倍速与原位恢复是独立能力，不从 URL 或一个 seek 标志推断。
PlaybackCapabilities initialPlaybackCapabilities(PlaybackSourceKind kind, bool duration_known) noexcept
{
    PlaybackCapabilities capabilities;
    capabilities.duration_known = duration_known && kind != PlaybackSourceKind::NetworkLive;
    switch (kind) {
        case PlaybackSourceKind::LocalFile:
            capabilities.can_seek = true;
            capabilities.can_change_speed = true;
            break;
        case PlaybackSourceKind::NetworkLive:
            capabilities.can_return_to_live = true;
            break;
        case PlaybackSourceKind::NetworkVod:
            break;
    }
    return capabilities;
}
