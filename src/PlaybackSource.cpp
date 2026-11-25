#include "PlaybackSource.h"

#include <QUrl>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
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
bool isLiveAddress(const std::string& address, PlaybackProtocol protocol)
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
    if (!url.isValid() || url.host().isEmpty() || url.port() == 0) return false;
    if (protocol == PlaybackProtocol::Srt) {
        return url.scheme() == "srt" && url.port() > 0 && url.userInfo().isEmpty() && url.path().isEmpty();
    }
    return url.scheme() == (protocol == PlaybackProtocol::Rtsp ? "rtsp" : "rtmp");
}

struct SrtUrlOptions {
    LiveSourceError error = LiveSourceError::None;
    SrtInputSettings settings;
};

// 与 FFmpeg 8 的逐项查询一致：键区分大小写，数字按十进制微秒；不解码/重写整个 URL。
// 重复、伪装键和非法数字直接拒绝，防止“已显式设置”与 FFmpeg 实际忽略产生两个解释。
SrtUrlOptions parseSrtOptions(std::string_view address)
{
    SrtUrlOptions result;
    const auto question = address.find('?');
    if (question == std::string_view::npos) return result;
    auto query = address.substr(question + 1);
    std::set<std::string> seen;
    bool latency_alias = false;
    while (!query.empty()) {
        const auto end = query.find('&');
        const auto item = query.substr(0, end);
        const auto equals = item.find('=');
        const auto raw_key = item.substr(0, equals);
        const auto value = equals == std::string_view::npos ? std::string_view{} : item.substr(equals + 1);
        auto key = QUrl::fromPercentEncoding(QByteArray(raw_key.data(), static_cast<int>(raw_key.size()))).toLower().toStdString();
        const bool controlled = key == "mode" || key == "transtype" || key == "latency" || key == "tsbpddelay" ||
            key == "rcvlatency" || key == "peerlatency";
        if (controlled) {
            if (raw_key != key || !seen.insert(key).second) {
                result.error = LiveSourceError::InvalidSrtOption; return result;
            }
            if (key == "mode" || key == "transtype") {
                if (value != (key == "mode" ? "caller" : "live")) {
                    result.error = LiveSourceError::UnsupportedSrtMode; return result;
                }
            } else {
                if (value.empty()) { result.error = LiveSourceError::InvalidSrtOption; return result; }
                std::int64_t micros = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), micros);
                // libsrt 最终用有符号 int 毫秒；在窄化之前拒绝溢出，不接受 strtoll 的部分数字。
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    micros < -1 || micros / 1000 > std::numeric_limits<int>::max()) {
                    result.error = LiveSourceError::InvalidSrtOption; return result;
                }
                result.settings.latency_from_url = true;
                const auto duration = std::chrono::microseconds{micros};
                if (key == "rcvlatency") result.settings.receive_latency = duration;
                else if (key == "peerlatency") result.settings.peer_latency = duration;
                else {
                    if (result.settings.latency || latency_alias) { result.error = LiveSourceError::InvalidSrtOption; return result; }
                    result.settings.latency = duration;
                    latency_alias = key == "tsbpddelay";
                }
            }
        }
        if (end == std::string_view::npos) break;
        query.remove_prefix(end + 1);
    }
    return result;
}

LiveSourceError validateLiveAddress(const std::string& address)
{
    const auto protocol = protocolOf(address);
    if (protocol != PlaybackProtocol::Rtsp && protocol != PlaybackProtocol::Rtmp && protocol != PlaybackProtocol::Srt)
        return address.find("://") == std::string::npos ? LiveSourceError::InvalidAddress : LiveSourceError::UnsupportedProtocol;
    if (!isLiveAddress(address, protocol)) return LiveSourceError::InvalidAddress;
    return protocol == PlaybackProtocol::Srt ? parseSrtOptions(address).error : LiveSourceError::None;
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
                               std::string display_name, bool valid, LiveSourceOptions options, std::optional<SrtInputSettings> srt_options)
    : kind_(kind), address_(std::move(address)), display_name_(std::move(display_name)),
      valid_(valid), live_options_(options), srt_options_(srt_options)
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
    const bool valid_address = validateLiveAddress(url) == LiveSourceError::None &&
        (protocol != PlaybackProtocol::Rtsp || options.rtsp_transport == RtspTransport::Tcp || options.rtsp_transport == RtspTransport::Udp);
    const bool valid = valid_address && (options.preset == LivePreset::Stable || options.preset == LivePreset::LowLatency);
    const auto srt_options = valid && protocol == PlaybackProtocol::Srt
        ? std::optional<SrtInputSettings>{parseSrtOptions(url).settings} : std::nullopt;
    return PlaybackSource(PlaybackSourceKind::NetworkLive, std::move(url), protocolDisplayName(protocol), valid, options, srt_options);
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

// 直播入口只校验结构和本版支持的模式；原始地址按字节保存，凭据不进入错误文案。
LiveSourceValidation createLiveSource(std::string url, LiveSourceOptions options)
{
    const auto error = validateLiveAddress(url);
    if (error != LiveSourceError::None) return {{}, error};
    auto source = PlaybackSource::networkLive(std::move(url), options);
    if (!source.isValid()) return {{}, LiveSourceError::InvalidAddress};
    return {std::move(source), LiveSourceError::None};
}

// 两套预设共用结构化校验；原始地址只进入内存源，继续/手动重接沿用保存的整组选项。
RtspSourceValidation createRtspSource(std::string url, LiveSourceOptions options)
{
    if (protocolOf(url) != PlaybackProtocol::Rtsp) {
        return {{}, url.find("://") == std::string::npos
            ? RtspSourceError::InvalidAddress : RtspSourceError::UnsupportedProtocol};
    }
    auto source = PlaybackSource::networkLive(std::move(url), options);
    if (!source.isValid()) return {{}, RtspSourceError::InvalidAddress};
    return {std::move(source), RtspSourceError::None};
}

// 保留稳定模式入口的兼容契约，默认仍启用有限重连。
RtspSourceValidation createStableRtspSource(std::string url, RtspTransport transport, bool auto_reconnect)
{
    return createRtspSource(std::move(url), {transport, LivePreset::Stable, auto_reconnect});
}

// 所有带单位的直播预设数值集中于此；稳定模式没有主动积压策略。
const LivePresetSettings& livePresetSettings(LivePreset preset) noexcept
{
    using namespace std::chrono_literals;
    static const LivePresetSettings stable{5s, 5s, 5s, 10s, 10s,
        {512, 2 * 1024 * 1024, 2s}, {512, 16 * 1024 * 1024, 2s},
        {350ms, {}}, {500ms, 512}, {}, {}, 1000ms, 1500000us, 1000000us};
    static const LivePresetSettings low_latency{5s, 5s, 3s, 10s, 10s,
        {256, 1024 * 1024, 750ms}, {256, 8 * 1024 * 1024, 750ms},
        {100ms, 350ms}, {100ms, 128}, 8, LiveBacklogSettings{}, 200ms, 400000us, 250000us};
    return preset == LivePreset::LowLatency ? low_latency : stable;
}

// 网络源共享期限设施，业务策略按三类源独立选择。
PlaybackPolicy playbackPolicy(const PlaybackSource& source) noexcept
{
    using namespace std::chrono_literals;
    PlaybackPolicy policy;
    switch (source.kind()) {
        case PlaybackSourceKind::LocalFile:
            break;
        case PlaybackSourceKind::NetworkLive: {
            const auto& settings = livePresetSettings(source.liveOptions()->preset);
            policy.pause = PauseBehavior::DisconnectLive;
            policy.end_of_stream = EndOfStreamBehavior::RecoverLive;
            policy.queue_full = QueueFullBehavior::ReconnectLive;
            policy.reconnect_position = ReconnectPosition::LiveEdge;
            policy.open_timeout = settings.open_timeout;
            policy.probe_timeout = settings.probe_timeout;
            policy.read_timeout = settings.read_timeout;
            policy.initial_output_timeout = settings.initial_output_timeout;
            policy.output_stall_timeout = settings.output_stall_timeout;
            policy.audio_queue_limits = settings.audio_queue;
            policy.video_queue_limits = settings.video_queue;
            policy.audio_buffer = settings.audio_buffer;
            policy.backlog = settings.backlog;
            policy.fps_probe_frames = settings.fps_probe_frames;
            if (source.protocol() == PlaybackProtocol::Rtsp) {
                if (source.liveOptions()->rtsp_transport == RtspTransport::Udp) policy.rtsp_udp = settings.rtsp_udp;
            } else if (source.protocol() == PlaybackProtocol::Rtmp) {
                policy.rtmp_buffer = settings.rtmp_buffer;
                policy.stream_probe_analysis = settings.stream_probe_analysis;
            } else if (source.protocol() == PlaybackProtocol::Srt) {
                policy.srt = source.srtOptions() ? *source.srtOptions() : SrtInputSettings{};
                // 任一显式延迟（包括旧别名）都阻止混入预设，未给出的值保持 FFmpeg 自身默认。
                if (!policy.srt->latency_from_url) policy.srt->latency = settings.srt_latency;
                policy.stream_probe_analysis = settings.stream_probe_analysis;
            }
            break;
        }
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
