#include "PlaybackSource.h"

#include <QUrl>

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
    if (path.empty() || path.find('\0') != std::string_view::npos) return false;
    // 防止包装后的 URL 被当成文件名显示。盘符允许 C:// 形式。
    if (path.find("://", 2) != std::string_view::npos) return false;
    const auto colon = path.find(':');
    if (colon == 1 && asciiLower(path[0]) >= 'a' && asciiLower(path[0]) <= 'z') {
        return true;
    }
    return colon == std::string_view::npos || colon > path.find_first_of("/\\");
}

bool hasRawControlOrSpace(std::string_view address) noexcept
{
    return std::any_of(address.begin(), address.end(), [](unsigned char value) {
        return value <= ' ' || value == 127;
    });
}

int hexValue(char value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    value = asciiLower(value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

// FFmpeg 可能解码鉴权转义。拒绝原始及转义的控制字符，但保留合法转义的原字节。
bool hasInvalidRtspEscape(std::string_view address) noexcept
{
    for (auto offset = address.find('%'); offset != std::string_view::npos;
         offset = address.find('%', offset + 3)) {
        if (address.size() - offset < 3) return true;
        const int high = hexValue(address[offset + 1]);
        const int low = hexValue(address[offset + 2]);
        if (high < 0 || low < 0) return true;
        const int decoded = 16 * high + low;
        if (decoded < ' ' || decoded == 127) return true;
    }
    return false;
}

// QUrl 负责 URL 结构检查；另外拒绝会被网络库省略或解释成其他字段的写法。
bool isRtspAddress(const std::string& address)
{
    if (hasRawControlOrSpace(address) || address.find_first_of("#\\") != std::string::npos ||
        hasInvalidRtspEscape(address)) {
        return false;
    }

    const auto rest = std::string_view(address).substr(address.find("://") + 3);
    auto authority = rest.substr(0, rest.find_first_of("/?"));
    if (const auto at = authority.find('@'); at != std::string_view::npos) {
        if (at == 0 || authority.front() == ':') return false;
        authority.remove_prefix(at + 1);
        if (authority.find('@') != std::string_view::npos) return false;
    }
    if (authority.empty() || authority.back() == ':') return false;
    // 百分号编码的普通主机名会由 QUrl 解码，FFmpeg 实际解析时却使用原始主机字节。
    if (authority.front() != '[' && authority.find('%') != std::string_view::npos) return false;

    const auto encoded = QByteArray::fromStdString(address);
    if (!encoded.isValidUtf8()) return false;
    const auto url = QUrl::fromEncoded(encoded, QUrl::StrictMode);
    return url.isValid() && url.scheme() == "rtsp" && !url.host().isEmpty() && url.port() != 0;
}

// 其他协议保留已建模的契约，协议各自的连接约束在开放入口时再补齐。
bool hasNetworkAuthority(std::string_view address, bool allow_userinfo) noexcept
{
    if (hasRawControlOrSpace(address)) return false;
    const auto rest = address.substr(address.find("://") + 3);
    const auto authority = rest.substr(0, rest.find_first_of("/?#"));
    const auto credentials = authority.rfind('@');
    return !authority.empty() && (credentials == std::string_view::npos ||
        (allow_userinfo && credentials + 1 < authority.size()));
}

} // namespace

// 保存工厂已经选定的源类型、地址和显示数据。
PlaybackSource::PlaybackSource(PlaybackSourceKind kind, std::string address,
                               std::string display_name, bool valid, LiveSourceOptions options)
    : kind_(kind), address_(std::move(address)), display_name_(std::move(display_name)),
      valid_(valid), live_options_(options)
{
}

// 提取跨斜杠形式的文件名，但原始路径不做编码或分隔符转换。
PlaybackSource PlaybackSource::localFile(std::string path, std::string display_name)
{
    const bool valid = isLocalPath(path);
    if (!valid) {
        // 误投到本地入口的 URL 连末段也可能含口令、令牌或签名。
        display_name = "Local file";
    } else if (display_name.empty()) {
        const auto separator = path.find_last_of("/\\");
        display_name = separator == std::string::npos ? path : path.substr(separator + 1);
    }
    return PlaybackSource(PlaybackSourceKind::LocalFile, std::move(path), std::move(display_name), valid);
}

// 从协议生成固定显示名，保留带有调用参数的原始地址供打开操作使用。
PlaybackSource PlaybackSource::networkLive(std::string url, LiveSourceOptions options)
{
    const auto protocol = protocolOf(url);
    const bool valid = protocol == PlaybackProtocol::Rtsp
        ? isRtspAddress(url) && (options.rtsp_transport == RtspTransport::Tcp || options.rtsp_transport == RtspTransport::Udp)
        : (protocol == PlaybackProtocol::Rtmp || protocol == PlaybackProtocol::Srt) && hasNetworkAuthority(url, true);
    return PlaybackSource(PlaybackSourceKind::NetworkLive, std::move(url), protocolDisplayName(protocol), valid, options);
}

// 点播不携带 RTSP 传输或直播预设。
PlaybackSource PlaybackSource::networkVod(std::string url)
{
    const auto protocol = protocolOf(url);
    const bool valid = (protocol == PlaybackProtocol::Http || protocol == PlaybackProtocol::Https) &&
                       hasNetworkAuthority(url, false);
    return PlaybackSource(PlaybackSourceKind::NetworkVod, std::move(url), protocolDisplayName(protocol), valid);
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

// 稳定模式默认启用有限重连；参数按播放项保存，继续和手动重接沿用同一配置。
RtspSourceValidation createStableRtspSource(std::string url, RtspTransport transport, bool auto_reconnect)
{
    if (protocolOf(url) != PlaybackProtocol::Rtsp) {
        return {{}, url.find("://") == std::string::npos
            ? RtspSourceError::InvalidAddress : RtspSourceError::UnsupportedProtocol};
    }
    auto source = PlaybackSource::networkLive(std::move(url), {transport, LivePreset::Stable, auto_reconnect});
    if (!source.isValid()) return {{}, RtspSourceError::InvalidAddress};
    return {std::move(source), RtspSourceError::None};
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
