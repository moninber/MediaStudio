/**
 * @file MediaDecoder.cpp
 * @brief 媒体解码器实现 - FFmpeg解封装与解码的具体实现
 */

#include "MediaDecoder.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>

namespace {

std::atomic<unsigned> playback_ffmpeg_users{0};
constexpr long double TimestampUpperBound = 0x1p63L;

// 流时间基无效时，解码器继续使用原有的时间戳回退路径。
bool hasValidTimeBase(AVRational time_base) noexcept
{
    return time_base.num > 0 && time_base.den > 0;
}

// 保留时间戳是否来自媒体的区别，便于直播选用时钟锚点。
std::optional<double> decodedTimestamp(const AVFrame& frame, AVRational time_base)
{
    if (!hasValidTimeBase(time_base)) return {};
    int64_t ts = frame.best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = frame.pts;
    if (ts == AV_NOPTS_VALUE) ts = frame.pkt_dts;
    if (ts == AV_NOPTS_VALUE) return {};
    const double seconds = ts * av_q2d(time_base);
    return std::isfinite(seconds) ? std::optional<double>{seconds} : std::nullopt;
}

bool isCorrupt(const AVFrame& frame) noexcept
{
    return (frame.flags & AV_FRAME_FLAG_CORRUPT) || frame.decode_error_flags != 0;
}

// RTSP 对端文本和本地清单中的嵌套 URL 都可能含凭据，即便协议检查最终拒绝打开。
// 涉及 FFmpeg 内部线程，播放输入存活期间只保留应用自己输出的诊断。
void ffmpegLogCallback(void* context, int level, const char* format, va_list args)
{
    if (playback_ffmpeg_users.load(std::memory_order_acquire) == 0) {
        av_log_default_callback(context, level, format, args);
    }
}

// 将打开各阶段的失败区分为主动取消、期限到达和普通 FFmpeg 错误。
MediaOpenResult classifyOpenResult(int error, MediaIoStopReason reason, MediaOpenStage stage)
{
    if (reason == MediaIoStopReason::Cancelled) {
        return {MediaOpenStatus::Cancelled, error, stage};
    }
    if (reason == MediaIoStopReason::Timeout || error == AVERROR(ETIMEDOUT)) {
        return {MediaOpenStatus::Timeout, error, stage};
    }
    return {error < 0 ? MediaOpenStatus::Error : MediaOpenStatus::Opened, error, stage};
}

// FFmpeg 按小写识别协议，只规范化已经验证的 scheme，保留地址其余字节。
std::string normalizeFfmpegNetworkScheme(std::string address)
{
    for (char& value : address) {
        if (value == ':') {
            break;
        }
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value + ('a' - 'A'));
        }
    }
    return address;
}

// 打开选中流的解码器，失败时回收未完成的上下文并保留原始错误。
int openStreamDecoder(const AVCodecParameters* params, AVCodecContext*& context)
{
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        return AVERROR_DECODER_NOT_FOUND;
    }
    context = avcodec_alloc_context3(codec);
    if (!context) {
        return AVERROR(ENOMEM);
    }
    int ret = avcodec_parameters_to_context(context, params);
    if (ret >= 0) {
        ret = avcodec_open2(context, codec, nullptr);
    }
    if (ret < 0) {
        avcodec_free_context(&context);
    }
    return ret;
}

} // namespace

// 外部调用者（PlayerController）
// │
// ├── open(filename)
// │   ├── avformat_open_input()       打开文件
// │   ├── avformat_find_stream_info() 探测流
// │   ├── avcodec_find_decoder()      查找解码器
// │   ├── avcodec_open2()             打开解码器
// │   └── extractMediaInfo()          填充 media_info_
// │
// ├── setPlaybackSpeed(speed)
// │   ├── destroyAudioFilterGraph()   销毁旧滤镜图
// │   ├── buildAtempoFilterChain()    构造 atempo 滤镜字符串
// │   │   └── 返回 "atempo=2.0" 等字符串
// │   └── initAudioFilterGraph()      重建滤镜图（变速不变调）
// │
// ├── seek(seconds)
// │   ├── av_seek_frame()             跳转到关键帧
// │   ├── avcodec_flush_buffers()     清空解码器缓冲
// │   └── 重置 eof/flush 状态标志
// │
// └── close()
//     ├── destroyAudioFilterGraph()
//     ├── avcodec_free_context()      释放视频/音频解码器
//     ├── sws_freeContext()
//     ├── swr_free()
//     └── avformat_close_input()

// ============================================================
// 构造函数：初始化所有指针为nullptr
// ============================================================
MediaDecoder::MediaDecoder()
    : format_ctx_(nullptr)
    , video_codec_ctx_(nullptr)
    , audio_codec_ctx_(nullptr)
    , sws_ctx_(nullptr)
    , swr_ctx_(nullptr)
    , audio_filter_graph_(nullptr)
    , audio_buffer_src_ctx_(nullptr)
    , audio_buffer_sink_ctx_(nullptr)
    , video_stream_index_(-1)
    , audio_stream_index_(-1)
    , current_time_(0.0)
    , playback_speed_(1.0f)
    , audio_drained_(false)
    , audio_filter_flush_sent_(false)
    , last_video_pts_(0.0)
    , has_last_video_pts_(false)
{
    // 应用在启动时安装一次进程日志入口，不在并行 I/O 期间切换全局级别或回调。
    static std::once_flag install_ffmpeg_log;
    std::call_once(install_ffmpeg_log, [] { av_log_set_callback(ffmpegLogCallback); });
}

// ============================================================
// 析构函数：确保资源释放
// ============================================================
MediaDecoder::~MediaDecoder()
{
    close();
}

// ============================================================
// 保留以文件路径打开本地媒体的兼容入口。
// ============================================================
bool MediaDecoder::open(const std::string& filename)
{
    return open(PlaybackSource::localFile(filename)).status == MediaOpenStatus::Opened;
}

// 打开并探测显式类型的源；每次网络操作单独设期限，取消状态贯穿整个尝试。
MediaOpenResult MediaDecoder::open(const PlaybackSource& source, std::shared_ptr<MediaIoControl> io)
{
    close();
    if (!source.isValid()) {
        return {MediaOpenStatus::Error, AVERROR(EINVAL), MediaOpenStage::Validation};
    }
    const bool local_file = source.kind() == PlaybackSourceKind::LocalFile;
    const auto protocol = source.protocol();
    const bool rtsp = !local_file && protocol == PlaybackProtocol::Rtsp;
    const bool rtmp = !local_file && protocol == PlaybackProtocol::Rtmp;
    const bool srt = !local_file && protocol == PlaybackProtocol::Srt;
    if (!local_file && (source.kind() != PlaybackSourceKind::NetworkLive || (!rtsp && !rtmp && !srt))) {
        return {MediaOpenStatus::Error, AVERROR(ENOSYS), MediaOpenStage::Validation};
    }

    // RTMP 的 FLV、SRT 的 MPEG-TS 与 RTSP 共用后续解码链路，不探测出嵌套网络清单。
    const auto* input_format = local_file ? nullptr : av_find_input_format(rtsp ? "rtsp" : rtmp ? "flv" : "mpegts");
    if (!local_file && !input_format) {
        return {MediaOpenStatus::Error, AVERROR_DEMUXER_NOT_FOUND, MediaOpenStage::Validation};
    }
    if (rtmp || srt) {
        void* cursor = nullptr;
        bool available = false;
        while (const char* name = avio_enum_protocols(&cursor, 0)) {
            if (std::strcmp(name, rtmp ? "rtmp" : "srt") == 0) available = true;
        }
        if (!available) return {MediaOpenStatus::Error, AVERROR_PROTOCOL_NOT_FOUND, MediaOpenStage::Validation};
    }

    const PlaybackPolicy policy = playbackPolicy(source);
    const std::string network_address = local_file ? std::string{} : normalizeFfmpegNetworkScheme(source.address());
    const char* input_address = local_file ? source.address().c_str() : network_address.c_str();
    io_ = io ? std::move(io) : std::make_shared<MediaIoControl>();
    playback_ffmpeg_users.fetch_add(1, std::memory_order_acq_rel);
    network_input_ = !local_file;
    read_timeout_ = policy.read_timeout;

    // 用安全显示名和原始错误号记录失败，关闭时保留 FFmpeg 回调对象。
    const auto fail = [&](MediaOpenResult result) {
        std::cerr << "[MediaDecoder] 打开未完成: " << source.displayName()
                  << ", 阶段=" << static_cast<int>(result.stage)
                  << ", FFmpeg=" << result.ffmpegError << std::endl;
        close();
        return result;
    };

    if (io_->isCancelled()) {
        return fail({MediaOpenStatus::Cancelled, AVERROR_EXIT, MediaOpenStage::OpenInput});
    }

    // 各协议只放行自己的传输依赖；RTMP 始终为客户端，不设置会隐式打开监听的 timeout。
    AVDictionary* options = nullptr;
    const bool udp = rtsp && source.liveOptions()->rtsp_transport == RtspTransport::Udp;
    const char* allowed_protocols = local_file ? "file" : rtmp ? "rtmp,tcp" : srt ? "srt" : udp ? "tcp,udp,rtp" : "tcp";
    int options_result = av_dict_set(&options, "protocol_whitelist", allowed_protocols, 0);
    if (options_result >= 0 && rtsp) {
        options_result = av_dict_set(&options, "rtsp_transport", udp ? "udp" : "tcp", 0);
    }
    if (options_result >= 0 && policy.rtsp_udp) {
        options_result = av_dict_set_int(&options, "max_delay", policy.rtsp_udp->max_delay.count(), 0);
        if (options_result >= 0) options_result = av_dict_set_int(&options, "reorder_queue_size", policy.rtsp_udp->reorder_packets, 0);
    }
    const auto put = [&](const char* name, const char* value) {
        if (options_result >= 0) options_result = av_dict_set(&options, name, value, 0);
    };
    if (policy.rtmp_buffer) {
        if (options_result >= 0) options_result = av_dict_set_int(&options, "rtmp_buffer", policy.rtmp_buffer->count(), 0);
        put("rtmp_live", "live");
        put("listen", "0");
    }
    if (policy.srt) {
        put("mode", "caller");
        put("transtype", "live");
        // URL 已含任一延迟值时完全不混入预设；保持原始查询给 FFmpeg 按自身语义解析。
        if (!policy.srt->latency_from_url && options_result >= 0)
            options_result = av_dict_set_int(&options, "latency", policy.srt->latency->count(), 0);
    }
    if (options_result < 0) {
        av_dict_free(&options);
        return fail(classifyOpenResult(options_result, io_->stopReason(), MediaOpenStage::OpenInput));
    }

    // 打开前分配格式上下文并挂上回调，涵盖连接及读取文件头的阻塞。
    format_ctx_ = avformat_alloc_context();
    if (!format_ctx_) {
        av_dict_free(&options);
        return fail(classifyOpenResult(AVERROR(ENOMEM), io_->stopReason(), MediaOpenStage::OpenInput));
    }
    format_ctx_->interrupt_callback = {MediaIoControl::interruptCallback, io_.get()};
    io_->armDeadline(policy.open_timeout);
    const int open_ret = avformat_open_input(&format_ctx_, input_address, input_format, &options);
    av_dict_free(&options);
    const auto open_result = classifyOpenResult(open_ret, io_->stopReason(), MediaOpenStage::OpenInput);
    if (open_result.status != MediaOpenStatus::Opened) {
        return fail(open_result);
    }
    if (policy.rtsp_udp) {
        std::int64_t reorder_packets = 0;
        const int queried = av_opt_get_int(format_ctx_->priv_data, "reorder_queue_size", 0, &reorder_packets);
        if (queried < 0 || format_ctx_->max_delay != policy.rtsp_udp->max_delay.count() ||
            reorder_packets != policy.rtsp_udp->reorder_packets) {
            return fail({MediaOpenStatus::Error, queried < 0 ? queried : AVERROR(EINVAL), MediaOpenStage::OpenInput});
        }
        udp_receive_settings_ = RtspUdpReceiveSettings{std::chrono::microseconds{format_ctx_->max_delay}, static_cast<int>(reorder_packets)};
    }
    if (rtmp || srt) {
        // 使用公开 AVOption 子对象搜索核对实际运行库；不依赖 FFmpeg 私有 URLContext 结构。
        const auto matches = [&](const char* name, std::int64_t wanted) {
            std::int64_t actual = 0;
            return format_ctx_->pb && av_opt_get_int(format_ctx_->pb, name, AV_OPT_SEARCH_CHILDREN, &actual) >= 0 && actual == wanted;
        };
        LiveInputReport report;
        if (rtmp) {
            if (!matches("rtmp_buffer", policy.rtmp_buffer->count()) || !matches("rtmp_live", -1) || !matches("listen", 0))
                return fail({MediaOpenStatus::Error, AVERROR(EINVAL), MediaOpenStage::OpenInput});
            report.rtmp_buffer = policy.rtmp_buffer;
        } else {
            const auto expected = [](const std::optional<std::chrono::microseconds>& value) { return value ? value->count() : -1; };
            if (!matches("mode", 0) || !matches("transtype", 0) || !matches("latency", expected(policy.srt->latency)) ||
                !matches("rcvlatency", expected(policy.srt->receive_latency)) || !matches("peerlatency", expected(policy.srt->peer_latency)))
                return fail({MediaOpenStatus::Error, AVERROR(EINVAL), MediaOpenStage::OpenInput});
            report.srt = policy.srt;
        }
        live_input_report_ = report;
    }

    // 探测可能继续读数据，独立于打开阶段重新计时。
    // 低延迟三种直播均减少帧率估算样本；仍完整探测格式/轨道并保留探测包，不丢参考帧。
    if (policy.fps_probe_frames) format_ctx_->fps_probe_size = *policy.fps_probe_frames;
    // FLV/TS 的默认长分析会把数秒音频积存在探测缓存，随后瞬发顶满直播队列。
    // 只约束这两个已选定容器的分析媒体时长，保留已读包和正常轨道探测，不改本地/RTSP。
    if (policy.stream_probe_analysis) {
        format_ctx_->max_analyze_duration = policy.stream_probe_analysis->count();
        live_input_report_->probe_analysis = std::chrono::microseconds{format_ctx_->max_analyze_duration};
    }
    io_->armDeadline(policy.probe_timeout);
    const int probe_ret = avformat_find_stream_info(format_ctx_, nullptr);
    const auto probe_result = classifyOpenResult(probe_ret, io_->stopReason(), MediaOpenStage::ProbeStreams);
    if (probe_result.status != MediaOpenStatus::Opened) {
        return fail(probe_result);
    }
    io_->clearDeadline();

    // --------------------------------------------------------
    // 步骤3：查找视频流和音频流
    // 一个媒体文件可能包含多个流（视频、音频、字幕等）
    // 我们需要找到第一个视频流和第一个音频流
    // --------------------------------------------------------
    video_stream_index_ = -1;
    audio_stream_index_ = -1;

    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        AVCodecParameters* codec_params = format_ctx_->streams[i]->codecpar;

        if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index_ == -1) {
            video_stream_index_ = i;
        }
        else if (codec_params->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index_ == -1) {
            audio_stream_index_ = i;
        }
    }

    // --------------------------------------------------------
    // 步骤4：打开视频解码器
    // 流程：查找解码器 → 创建上下文 → 复制参数 → 打开解码器
    // --------------------------------------------------------
    if (video_stream_index_ != -1) {
        const int ret = openStreamDecoder(
            format_ctx_->streams[video_stream_index_]->codecpar, video_codec_ctx_);
        if (ret < 0) {
            return fail(classifyOpenResult(ret, io_->stopReason(), MediaOpenStage::InitializeDecoders));
        }
        media_info_.has_video = true;
    }

    // --------------------------------------------------------
    // 步骤5：打开音频解码器（流程同视频）
    // --------------------------------------------------------
    int audio_ret = AVERROR_STREAM_NOT_FOUND;
    if (audio_stream_index_ != -1) {
        audio_ret = openStreamDecoder(
            format_ctx_->streams[audio_stream_index_]->codecpar, audio_codec_ctx_);
        if (audio_ret >= 0) {
            // 步骤6：将不同源的音频统一转换为 44100 Hz、立体声、16 位 PCM。
            AVChannelLayout dst_ch_layout;
            av_channel_layout_default(&dst_ch_layout, TARGET_CHANNELS);
            audio_ret = swr_alloc_set_opts2(&swr_ctx_, &dst_ch_layout, TARGET_SAMPLE_FMT,
                TARGET_SAMPLE_RATE, &audio_codec_ctx_->ch_layout, audio_codec_ctx_->sample_fmt,
                audio_codec_ctx_->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&dst_ch_layout);

            if (audio_ret >= 0) {
                audio_ret = swr_ctx_ ? swr_init(swr_ctx_) : AVERROR(ENOMEM);
            }
            if (audio_ret >= 0) {
                media_info_.has_audio = true;
                if (!initAudioFilterGraph()) {
                    audio_ret = AVERROR_UNKNOWN;
                }
            }
        }
        if (audio_ret < 0) {
            // 保留旧的纯视频降级路径；只有完全没有可播放流时打开失败。
            std::cerr << "[MediaDecoder] 无法初始化音频, FFmpeg=" << audio_ret << std::endl;
            destroyAudioFilterGraph();
            swr_free(&swr_ctx_);
            avcodec_free_context(&audio_codec_ctx_);
            media_info_.has_audio = false;
            audio_stream_index_ = -1;
        }
    }
    if (!media_info_.has_video && !media_info_.has_audio) {
        return fail(classifyOpenResult(audio_ret, io_->stopReason(), MediaOpenStage::InitializeDecoders));
    }

    // 网络地址可能包含口令或签名，不进入可展示和入库的 MediaInfo。
    if (local_file) {
        media_info_.file_path = source.address();
    }
    media_info_.file_name = source.displayName();
    audio_drained_ = !media_info_.has_audio;
    audio_filter_flush_sent_ = false;
    extractMediaInfo(local_file);

    if (io_->isCancelled()) {
        return fail({MediaOpenStatus::Cancelled, AVERROR_EXIT, MediaOpenStage::InitializeDecoders});
    }

    // 在启动解码线程之前复制时间基，读包即使修改格式上下文也不影响解码线程访问。
    if (video_stream_index_ >= 0) {
        video_time_base_ = format_ctx_->streams[video_stream_index_]->time_base;
    }
    if (audio_stream_index_ >= 0) {
        audio_time_base_ = format_ctx_->streams[audio_stream_index_]->time_base;
    }

    std::cout << "[MediaDecoder] 媒体打开成功: " << source.displayName() << std::endl;
    if (media_info_.has_video) {
        std::cout << "  视频: " << media_info_.video_info.width << "x"
                  << media_info_.video_info.height << " @ "
                  << media_info_.video_info.frame_rate << "fps, "
                  << media_info_.video_info.codec_name << std::endl;
    }
    if (media_info_.has_audio) {
        std::cout << "  音频: " << media_info_.audio_info.sample_rate << "Hz, "
                  << media_info_.audio_info.channels << "ch, "
                  << media_info_.audio_info.codec_name << std::endl;
    }

    return {MediaOpenStatus::Opened, 0, MediaOpenStage::InitializeDecoders};
}

// ============================================================
// 关闭媒体并释放所有资源；回调对象最后释放。
// ============================================================
void MediaDecoder::close()
{
    // 释放顺序：后创建的先释放（与创建顺序相反）

    destroyAudioFilterGraph();

    if (swr_ctx_) {
        swr_free(&swr_ctx_);         // 释放音频重采样器
        swr_ctx_ = nullptr;
    }

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);    // 释放图像转换器
        sws_ctx_ = nullptr;
    }

    if (audio_codec_ctx_) {
        avcodec_free_context(&audio_codec_ctx_);  // 释放音频解码器上下文
        audio_codec_ctx_ = nullptr;
    }

    if (video_codec_ctx_) {
        avcodec_free_context(&video_codec_ctx_);  // 释放视频解码器上下文
        video_codec_ctx_ = nullptr;
    }

    if (format_ctx_) {
        // 播放 worker 已回收，RTSP 关闭才允许一次有界的 TEARDOWN 写入。
        // 取消保持粘性；FFmpeg 的 async TEARDOWN 发送后即关闭，不等待服务器回应。
        const bool rtsp = network_input_ && format_ctx_->iformat &&
            std::strcmp(format_ctx_->iformat->name, "rtsp") == 0;
        if (rtsp) io_->beginCloseDeadline(std::chrono::milliseconds{250});
        else io_->limitDeadline(read_timeout_);
        avformat_close_input(&format_ctx_);
    }
    // 打开时 io_ 与日志计数同时建立，封装和解码器销毁后才能恢复默认日志。
    if (io_) {
        io_.reset();
        playback_ffmpeg_users.fetch_sub(1, std::memory_order_acq_rel);
    }
    read_timeout_ = std::chrono::milliseconds{0};
    network_input_ = false;
    read_deadline_active_ = false;

    video_stream_index_ = -1;
    audio_stream_index_ = -1;
    video_time_base_ = {0, 1};
    udp_receive_settings_.reset();
    live_input_report_.reset();
    audio_time_base_ = {0, 1};
    current_time_ = 0.0;
    playback_speed_ = 1.0f;
    media_info_ = MediaInfo();  // 重置媒体信息
    audio_drained_ = false;
    audio_filter_flush_sent_ = false;
    min_audio_input_pts_ = AV_NOPTS_VALUE;
    last_video_pts_ = 0.0;
    has_last_video_pts_ = false;
}

bool MediaDecoder::setPlaybackSpeed(float speed)
{
    if (!std::isfinite(speed)) return false;
    if (network_input_) return std::fabs(speed - 1.0f) < 0.001f;
    float clamped_speed = std::max(0.25f, std::min(4.0f, speed));
    if (std::fabs(playback_speed_ - clamped_speed) < 0.001f) {
        return true;
    }

    playback_speed_ = clamped_speed;

    if (!media_info_.has_audio || !audio_codec_ctx_ || !swr_ctx_) {
        return true;
    }

    swr_close(swr_ctx_);
    if (swr_init(swr_ctx_) < 0) {
        std::cerr << "[MediaDecoder] 重新初始化音频重采样器失败" << std::endl;
        return false;
    }

    audio_drained_ = false;
    audio_filter_flush_sent_ = false;
    return initAudioFilterGraph();
}

bool MediaDecoder::initAudioFilterGraph()
{
    if (!audio_codec_ctx_ || !media_info_.has_audio) {
        return false;
    }

    destroyAudioFilterGraph();

    audio_filter_graph_ = avfilter_graph_alloc();
    if (!audio_filter_graph_) {
        return false;
    }

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffer || !abuffersink) {
        destroyAudioFilterGraph();
        return false;
    }

    AVChannelLayout output_layout;
    av_channel_layout_default(&output_layout, TARGET_CHANNELS);

    char layout_desc[64] = {};
    if (av_channel_layout_describe(&output_layout, layout_desc, sizeof(layout_desc)) < 0) {
        av_channel_layout_uninit(&output_layout);
        destroyAudioFilterGraph();
        return false;
    }

    char src_args[256] = {};
    snprintf(src_args, sizeof(src_args),
        "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
        TARGET_SAMPLE_RATE,
        TARGET_SAMPLE_RATE,
        av_get_sample_fmt_name(TARGET_SAMPLE_FMT),
        layout_desc);

    int ret = avfilter_graph_create_filter(&audio_buffer_src_ctx_, abuffer, "in",
        src_args, nullptr, audio_filter_graph_);
    if (ret < 0) {
        av_channel_layout_uninit(&output_layout);
        destroyAudioFilterGraph();
        return false;
    }

    ret = avfilter_graph_create_filter(&audio_buffer_sink_ctx_, abuffersink, "out",
        nullptr, nullptr, audio_filter_graph_);
    if (ret < 0) {
        av_channel_layout_uninit(&output_layout);
        destroyAudioFilterGraph();
        return false;
    }

    std::string filter_desc = buildAtempoFilterChain(playback_speed_);
    filter_desc += ",aformat=sample_fmts=s16:sample_rates=44100:channel_layouts=stereo";

    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        av_channel_layout_uninit(&output_layout);
        destroyAudioFilterGraph();
        return false;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = audio_buffer_src_ctx_;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = audio_buffer_sink_ctx_;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    ret = avfilter_graph_parse_ptr(audio_filter_graph_, filter_desc.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    av_channel_layout_uninit(&output_layout);
    if (ret < 0) {
        destroyAudioFilterGraph();
        return false;
    }

    ret = avfilter_graph_config(audio_filter_graph_, nullptr);
    if (ret < 0) {
        destroyAudioFilterGraph();
        return false;
    }

    audio_filter_flush_sent_ = false;
    return true;
}

void MediaDecoder::destroyAudioFilterGraph()
{
    if (audio_filter_graph_) {
        avfilter_graph_free(&audio_filter_graph_);
        audio_filter_graph_ = nullptr;
    }
    audio_buffer_src_ctx_ = nullptr;
    audio_buffer_sink_ctx_ = nullptr;
    audio_filter_flush_sent_ = false;
}

std::string MediaDecoder::buildAtempoFilterChain(float speed) const
{
    double remaining = std::max(0.25f, std::min(4.0f, speed));
    std::ostringstream filter_chain;
    filter_chain.setf(std::ios::fixed);
    filter_chain.precision(6);

    bool first_stage = true;
    while (remaining < 0.5 - 0.0001) {
        if (!first_stage) filter_chain << ",";
        filter_chain << "atempo=0.5";
        remaining /= 0.5;
        first_stage = false;
    }
    while (remaining > 2.0 + 0.0001) {
        if (!first_stage) filter_chain << ",";
        filter_chain << "atempo=2.0";
        remaining /= 2.0;
        first_stage = false;
    }

    if (!first_stage) filter_chain << ",";
    filter_chain << "atempo=" << remaining;
    return filter_chain.str();
}

bool MediaDecoder::pullAudioFilterFrame(DecodedFrame& out_frame)
{
    if (!audio_buffer_sink_ctx_ || audio_drained_) {
        return false;
    }

    AVFrame* filtered_frame = av_frame_alloc();
    if (!filtered_frame) {
        return false;
    }

    int ret = av_buffersink_get_frame(audio_buffer_sink_ctx_, filtered_frame);
    if (ret == AVERROR(EAGAIN)) {
        av_frame_free(&filtered_frame);
        return false;
    }
    if (ret == AVERROR_EOF) {
        audio_drained_ = true;
        av_frame_free(&filtered_frame);
        return false;
    }
    if (ret < 0) {
        av_frame_free(&filtered_frame);
        return false;
    }

    int data_size = av_samples_get_buffer_size(nullptr, TARGET_CHANNELS,
        filtered_frame->nb_samples, TARGET_SAMPLE_FMT, 1);
    if (data_size <= 0) {
        av_frame_free(&filtered_frame);
        return false;
    }

    uint8_t* output_buffer = (uint8_t*)av_malloc(data_size);
    if (!output_buffer) {
        av_frame_free(&filtered_frame);
        return false;
    }

    memcpy(output_buffer, filtered_frame->data[0], data_size);

    out_frame.type = DecodedFrame::Type::Audio;
    out_frame.data = output_buffer;
    out_frame.samples = filtered_frame->nb_samples;
    out_frame.data_size = data_size;
    out_frame.pts = (filtered_frame->pts != AV_NOPTS_VALUE)
        ? (double)filtered_frame->pts / TARGET_SAMPLE_RATE
        : current_time_.load();
    current_time_ = out_frame.pts;

    av_frame_free(&filtered_frame);
    return true;
}

// 兼容原有调用者，只在读到选中包时返回 true。
bool MediaDecoder::readPacket(AVPacket* packet)
{
    return readPacketResult(packet).status == MediaReadStatus::Packet;
}

// 对一次选中音视频包的读取计时，Again 和非选中包都不会重置期限。
MediaReadResult MediaDecoder::readPacketResult(AVPacket* packet)
{
    if (!packet) {
        return classifyMediaReadResult(AVERROR(EINVAL));
    }
    av_packet_unref(packet);
    if (!format_ctx_) {
        return classifyMediaReadResult(AVERROR(EINVAL));
    }

    if (!read_deadline_active_) {
        io_->armDeadline(read_timeout_);
        read_deadline_active_ = true;
    }
    while (true) {
        const auto reason = io_->stopReason();
        if (reason != MediaIoStopReason::None) {
            const int error = reason == MediaIoStopReason::Cancelled ? AVERROR_EXIT : AVERROR(ETIMEDOUT);
            return classifyMediaReadResult(error, reason);
        }

        const int ret = av_read_frame(format_ctx_, packet);
        const auto result = classifyMediaReadResult(ret, io_->stopReason());
        if (result.status != MediaReadStatus::Packet) {
            av_packet_unref(packet);
            return result;
        }
        const bool selected = isVideoPacket(packet) || isAudioPacket(packet);
        const bool valid_payload = !network_input_ || (packet->data && packet->size > 0);
        if (selected && valid_payload) {
            io_->clearDeadline();
            read_deadline_active_ = false;
            return result;
        }
        av_packet_unref(packet);
    }
}

// 读包线程按打开时选定的流索引分流。
bool MediaDecoder::isVideoPacket(const AVPacket* packet) const
{
    return packet && video_stream_index_ >= 0 && packet->stream_index == video_stream_index_;
}

// 音频初始化失败时索引为 -1，读包线程跳过这一路。
bool MediaDecoder::isAudioPacket(const AVPacket* packet) const
{
    return packet && audio_stream_index_ >= 0 && packet->stream_index == audio_stream_index_;
}

bool MediaDecoder::decodeVideoPacket(const AVPacket* packet, std::vector<DecodedFrame>& out_frames)
{
    if (!video_codec_ctx_ || !packet) {
        return false;
    }

    int ret = avcodec_send_packet(video_codec_ctx_, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return false;
    }

    receiveVideoFrames(out_frames);
    return true;
}

bool MediaDecoder::decodeAudioPacket(const AVPacket* packet, std::vector<DecodedFrame>& out_frames)
{
    if (!audio_codec_ctx_ || !packet) {
        return false;
    }

    DecodedFrame filtered_frame;
    while (pullAudioFilterFrame(filtered_frame)) {
        out_frames.push_back(filtered_frame);
        filtered_frame = DecodedFrame();
    }

    int ret = avcodec_send_packet(audio_codec_ctx_, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return false;
    }

    receiveAudioFrames(out_frames);
    while (pullAudioFilterFrame(filtered_frame)) {
        out_frames.push_back(filtered_frame);
        filtered_frame = DecodedFrame();
    }
    return true;
}

bool MediaDecoder::flushVideoDecoder(std::vector<DecodedFrame>& out_frames)
{
    if (!video_codec_ctx_) {
        return false;
    }

    avcodec_send_packet(video_codec_ctx_, nullptr);
    receiveVideoFrames(out_frames);
    return true;
}

bool MediaDecoder::flushAudioDecoder(std::vector<DecodedFrame>& out_frames)
{
    if (!audio_codec_ctx_) {
        return false;
    }

    DecodedFrame filtered_frame;
    while (pullAudioFilterFrame(filtered_frame)) {
        out_frames.push_back(filtered_frame);
        filtered_frame = DecodedFrame();
    }

    avcodec_send_packet(audio_codec_ctx_, nullptr);
    receiveAudioFrames(out_frames);

    if (audio_buffer_src_ctx_ && !audio_filter_flush_sent_) {
        av_buffersrc_add_frame_flags(audio_buffer_src_ctx_, nullptr, 0);
        audio_filter_flush_sent_ = true;
    }
    while (pullAudioFilterFrame(filtered_frame)) {
        out_frames.push_back(filtered_frame);
        filtered_frame = DecodedFrame();
    }

    audio_drained_ = true;
    return true;
}

void MediaDecoder::receiveVideoFrames(std::vector<DecodedFrame>& out_frames)
{
    if (!video_codec_ctx_) {
        return;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return;
    }

    while (true) {
        int ret = avcodec_receive_frame(video_codec_ctx_, frame);
        if (ret == 0) {
            DecodedFrame out_frame;
            if (convertVideoFrame(frame, out_frame)) {
                out_frames.push_back(out_frame);
            }
            av_frame_unref(frame);
            continue;
        }
        break;
    }

    av_frame_free(&frame);
}

void MediaDecoder::receiveAudioFrames(std::vector<DecodedFrame>& out_frames)
{
    if (!audio_codec_ctx_) {
        return;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return;
    }

    while (true) {
        int ret = avcodec_receive_frame(audio_codec_ctx_, frame);
        if (ret == 0) {
            DecodedFrame out_frame;
            if (convertAudioFrame(frame, out_frame)) {
                out_frames.push_back(out_frame);
            }
            av_frame_unref(frame);
            continue;
        }
        break;
    }

    av_frame_free(&frame);
}

// ============================================================
// 转换视频帧，使用打开时缓存的时间基计算 PTS。
// ============================================================
bool MediaDecoder::convertVideoFrame(AVFrame* frame, DecodedFrame& out_frame)
{
    if (!frame || !video_codec_ctx_ || video_stream_index_ == -1) {
        return false;
    }

    if (network_input_ && isCorrupt(*frame)) {
        out_frame.type = DecodedFrame::Type::Video;
        out_frame.corrupt = true;
        return true;
    }
    if (network_input_) {
        const auto rate = video_codec_ctx_->framerate;
        if (rate.num > 0 && rate.den > 0) {
            const double value = av_q2d(rate);
            if (std::isfinite(value) && value > 0 && value <= 120) out_frame.codec_frame_rate = value;
        }
        if (const auto* name = av_get_pix_fmt_name(video_codec_ctx_->pix_fmt)) out_frame.source_pixel_format = name;
    }

    const double fps = media_info_.video_info.frame_rate > 0.0
        ? media_info_.video_info.frame_rate
        : 25.0;
    double pts = 0.0;
    const auto timestamp = decodedTimestamp(*frame, video_time_base_);
    if (timestamp) {
        pts = *timestamp;
        out_frame.has_valid_pts = true;
        if (!media_info_.has_audio && has_last_video_pts_ && pts <= last_video_pts_) {
            pts = last_video_pts_ + 1.0 / fps;
            out_frame.has_valid_pts = false;
        }
    }
    else {
        pts = has_last_video_pts_ ? last_video_pts_ + 1.0 / fps : 0.0;
    }
    last_video_pts_ = pts;
    has_last_video_pts_ = true;

    if (frame->format == AV_PIX_FMT_YUV420P) {
        const int y_size = frame->width * frame->height;
        const int chroma_width = (frame->width + 1) / 2;
        const int chroma_height = (frame->height + 1) / 2;
        const int chroma_size = chroma_width * chroma_height;
        const int buf_size = y_size + chroma_size * 2;
        uint8_t* yuv_buf = (uint8_t*)av_malloc(buf_size);
        if (!yuv_buf) {
            return false;
        }

        uint8_t* dst_y = yuv_buf;
        uint8_t* dst_u = dst_y + y_size;
        uint8_t* dst_v = dst_u + chroma_size;
        av_image_copy_plane(dst_y, frame->width, frame->data[0], frame->linesize[0],
            frame->width, frame->height);
        av_image_copy_plane(dst_u, chroma_width, frame->data[1], frame->linesize[1],
            chroma_width, chroma_height);
        av_image_copy_plane(dst_v, chroma_width, frame->data[2], frame->linesize[2],
            chroma_width, chroma_height);

        out_frame.type = DecodedFrame::Type::Video;
        out_frame.video_format = VideoFrameFormat::YUV420P;
        out_frame.data = yuv_buf;
        out_frame.planes[0] = dst_y;
        out_frame.planes[1] = dst_u;
        out_frame.planes[2] = dst_v;
        out_frame.linesizes[0] = frame->width;
        out_frame.linesizes[1] = chroma_width;
        out_frame.linesizes[2] = chroma_width;
        out_frame.video_full_range = (frame->color_range == AVCOL_RANGE_JPEG);
        out_frame.video_bt709 = (frame->width >= 1280 || frame->height >= 720);
        out_frame.data_size = buf_size;
        out_frame.width = frame->width;
        out_frame.height = frame->height;
        out_frame.pts = pts;
        current_time_ = pts;
        return true;
    }

    sws_ctx_ = sws_getCachedContext(
        sws_ctx_,
        frame->width, frame->height, (AVPixelFormat)frame->format,
        frame->width, frame->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        return false;
    }

    int src_full = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    const int* in_cs = (frame->width >= 1280 || frame->height >= 720)
        ? sws_getCoefficients(SWS_CS_ITU709)
        : sws_getCoefficients(SWS_CS_ITU601);
    sws_setColorspaceDetails(sws_ctx_, in_cs, src_full,
        sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);

    int buf_size = frame->width * frame->height * 4;
    uint8_t* rgba_buf = (uint8_t*)av_malloc(buf_size);
    if (!rgba_buf) {
        return false;
    }

    uint8_t* dst[4] = { rgba_buf, nullptr, nullptr, nullptr };
    int dst_stride[4] = { frame->width * 4, 0, 0, 0 };
    sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);

    out_frame.type = DecodedFrame::Type::Video;
    out_frame.video_format = VideoFrameFormat::RGBA;
    out_frame.data = rgba_buf;
    out_frame.planes[0] = rgba_buf;
    out_frame.linesizes[0] = frame->width * 4;
    out_frame.video_full_range = true;
    out_frame.video_bt709 = false;
    out_frame.data_size = buf_size;
    out_frame.width = frame->width;
    out_frame.height = frame->height;
    out_frame.pts = pts;
    current_time_ = pts;
    return true;
}

// ============================================================
// 转换音频帧，使用缓存的时间基建立直出和 atempo 的时间戳。
// ============================================================
bool MediaDecoder::convertAudioFrame(AVFrame* frame, DecodedFrame& out_frame)
{
    if (!frame || !swr_ctx_ || !audio_buffer_src_ctx_ || audio_stream_index_ == -1) {
        return false;
    }
    if (network_input_ && isCorrupt(*frame)) {
        out_frame.type = DecodedFrame::Type::Audio;
        out_frame.corrupt = true;
        return true;
    }

    int output_samples = (int)av_rescale_rnd(
        swr_get_delay(swr_ctx_, frame->sample_rate) + frame->nb_samples,
        TARGET_SAMPLE_RATE, frame->sample_rate, AV_ROUND_UP);
    if (output_samples <= 0) {
        return false;
    }

    AVFrame* resampled_frame = av_frame_alloc();
    if (!resampled_frame) {
        return false;
    }

    resampled_frame->format = TARGET_SAMPLE_FMT;
    resampled_frame->sample_rate = TARGET_SAMPLE_RATE;
    av_channel_layout_default(&resampled_frame->ch_layout, TARGET_CHANNELS);
    resampled_frame->nb_samples = output_samples;
    if (av_frame_get_buffer(resampled_frame, 0) < 0) {
        av_channel_layout_uninit(&resampled_frame->ch_layout);
        av_frame_free(&resampled_frame);
        return false;
    }

    // 定位后的输出可能从重采样器缓存开始，记录转换前的延迟（按输出采样计）。
    const int64_t output_delay = min_audio_input_pts_ == AV_NOPTS_VALUE
        ? 0 : std::max<int64_t>(0, swr_get_delay(swr_ctx_, TARGET_SAMPLE_RATE));
    int converted_samples = swr_convert(swr_ctx_,
        resampled_frame->data, output_samples,
        (const uint8_t**)frame->data, frame->nb_samples);
    if (converted_samples <= 0) {
        av_channel_layout_uninit(&resampled_frame->ch_layout);
        av_frame_free(&resampled_frame);
        return false;
    }

    resampled_frame->nb_samples = converted_samples;

    if (std::fabs(playback_speed_ - 1.0f) < 0.001f) {
        int data_size = av_samples_get_buffer_size(nullptr, TARGET_CHANNELS,
            converted_samples, TARGET_SAMPLE_FMT, 1);
        if (data_size <= 0) {
            av_channel_layout_uninit(&resampled_frame->ch_layout);
            av_frame_free(&resampled_frame);
            return false;
        }

        uint8_t* output_buffer = (uint8_t*)av_malloc(data_size);
        if (!output_buffer) {
            av_channel_layout_uninit(&resampled_frame->ch_layout);
            av_frame_free(&resampled_frame);
            return false;
        }

        memcpy(output_buffer, resampled_frame->data[0], data_size);

        out_frame.type = DecodedFrame::Type::Audio;
        out_frame.data = output_buffer;
        out_frame.samples = converted_samples;
        out_frame.data_size = data_size;
        const auto timestamp = decodedTimestamp(*frame, audio_time_base_);
        out_frame.has_valid_pts = timestamp.has_value();
        out_frame.pts = timestamp.value_or(current_time_.load());
        current_time_ = out_frame.pts;

        av_channel_layout_uninit(&resampled_frame->ch_layout);
        av_frame_free(&resampled_frame);
        return true;
    }

    int64_t src_ts = frame->best_effort_timestamp;
    if (src_ts == AV_NOPTS_VALUE) src_ts = frame->pts;
    if (src_ts == AV_NOPTS_VALUE) src_ts = frame->pkt_dts;
    resampled_frame->pts = AV_NOPTS_VALUE;
    if (src_ts != AV_NOPTS_VALUE && hasValidTimeBase(audio_time_base_)) {
        const AVRational sample_time_base{1, TARGET_SAMPLE_RATE};
        const auto rounding = min_audio_input_pts_ == AV_NOPTS_VALUE ? AV_ROUND_NEAR_INF : AV_ROUND_DOWN;
        const auto pts = av_rescale_q_rnd(src_ts, audio_time_base_, sample_time_base, rounding);
        if (pts > AV_NOPTS_VALUE + output_delay) {
            resampled_frame->pts = pts - output_delay;
        }
    }

    // 在变速前裁剪已知时间戳的预解码采样。移动 packed PCM 的内容以保留 AVFrame 的缓冲区所有权。
    if (min_audio_input_pts_ != AV_NOPTS_VALUE && resampled_frame->pts != AV_NOPTS_VALUE &&
        resampled_frame->pts < min_audio_input_pts_) {
        if (resampled_frame->pts <= min_audio_input_pts_ - converted_samples) {
            av_frame_free(&resampled_frame);
            return false;
        }
        const auto skip = static_cast<int>(min_audio_input_pts_ - resampled_frame->pts);
        const std::size_t bytes_per_sample = TARGET_CHANNELS * av_get_bytes_per_sample(TARGET_SAMPLE_FMT);
        uint8_t* samples = resampled_frame->data[0];
        std::memmove(samples, samples + skip * bytes_per_sample,
                     (converted_samples - skip) * bytes_per_sample);
        resampled_frame->nb_samples = converted_samples - skip;
        resampled_frame->pts = min_audio_input_pts_;
    }

    int ret = av_buffersrc_add_frame_flags(audio_buffer_src_ctx_, resampled_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    av_channel_layout_uninit(&resampled_frame->ch_layout);
    av_frame_free(&resampled_frame);
    if (ret < 0) {
        return false;
    }

    return pullAudioFilterFrame(out_frame);
}

// ============================================================
// 定位成功后才刷新编解码器、重采样器和滤镜状态。
// ============================================================
bool MediaDecoder::seek(double seconds)
{
    if (!format_ctx_ || network_input_ || !std::isfinite(seconds) || seconds < 0.0) {
        return false;
    }

    // 音频是播放主时钟，优先按音频流定位能让听感上的 seek 落点更接近目标。
    const int seek_stream_index = (audio_stream_index_ != -1) ? audio_stream_index_ : video_stream_index_;
    if (seek_stream_index < 0 || static_cast<unsigned>(seek_stream_index) >= format_ctx_->nb_streams) {
        return false;
    }

    // 向下取整到流的时间单位，并检查整数范围，避免超大目标溢出。
    const AVRational time_base = format_ctx_->streams[seek_stream_index]->time_base;
    if (!hasValidTimeBase(time_base)) {
        return false;
    }
    const long double ticks = static_cast<long double>(seconds) * time_base.den / time_base.num;
    if (!std::isfinite(ticks) || ticks >= TimestampUpperBound) {
        return false;
    }
    const auto timestamp = static_cast<int64_t>(ticks);

    if (av_seek_frame(format_ctx_, seek_stream_index, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }
    flushAfterDiscontinuity(seconds);
    return true;
}

// 清除旧解码状态；无法重定位的本地源也可从当前位置切换播放速度。
void MediaDecoder::flushAfterDiscontinuity(double seconds)
{
    if ((!video_codec_ctx_ && !audio_codec_ctx_) || !std::isfinite(seconds) || seconds < 0.0) {
        return;
    }
    if (video_codec_ctx_) {
        avcodec_flush_buffers(video_codec_ctx_);
    }
    if (audio_codec_ctx_) {
        avcodec_flush_buffers(audio_codec_ctx_);
    }
    if (swr_ctx_) {
        swr_close(swr_ctx_);
        swr_init(swr_ctx_);
    }
    if (media_info_.has_audio) {
        initAudioFilterGraph();
    }

    audio_drained_ = !media_info_.has_audio;
    audio_filter_flush_sent_ = false;
    // 取不早于目标的第一个输出采样，下面的滤镜只处理这个位置及之后的 PCM。
    const long double first_sample = std::ceil(static_cast<long double>(seconds) * TARGET_SAMPLE_RATE);
    min_audio_input_pts_ = first_sample < TimestampUpperBound ? static_cast<int64_t>(first_sample) : AV_NOPTS_VALUE;
    last_video_pts_ = seconds;
    has_last_video_pts_ = true;

    current_time_ = seconds;
}

// ============================================================
// 提取媒体信息；网络源不额外查询文件大小，以免引入无期限的网络操作。
// ============================================================
void MediaDecoder::extractMediaInfo(bool localFile)
{
    if (!format_ctx_) return;

    // 文件格式信息
    media_info_.format_name = format_ctx_->iformat->name;
    media_info_.duration = (localFile && format_ctx_->duration > 0)
        ? static_cast<double>(format_ctx_->duration) / AV_TIME_BASE
        : 0.0;
    media_info_.file_size = localFile && format_ctx_->pb ? avio_size(format_ctx_->pb) : 0;
    media_info_.overall_bit_rate = format_ctx_->bit_rate;

    // 视频流信息
    if (video_stream_index_ != -1 && video_codec_ctx_) {
        AVStream* vstream = format_ctx_->streams[video_stream_index_];
        media_info_.video_info.width = video_codec_ctx_->width;
        media_info_.video_info.height = video_codec_ctx_->height;
        // r_frame_rate 对某些容器（如MP4）可能返回异常值（如1000fps）
        // 优先使用 avg_frame_rate，再用 r_frame_rate，最后fallback到25fps
        const auto valid_fps = [](double fps) { return std::isfinite(fps) && fps > 0.0 && fps <= 120.0; };
        double fps = av_q2d(vstream->avg_frame_rate);
        if (!valid_fps(fps)) fps = av_q2d(vstream->r_frame_rate);
        // 缺少 SPS 时即使库暂时给出一个猜测值，也等待实际解码后再确认直播元信息。
        media_info_.video_info.frame_rate_known = valid_fps(fps) && video_codec_ctx_->width > 0 && video_codec_ctx_->height > 0;
        if (!valid_fps(fps)) fps = 25.0;
        media_info_.video_info.frame_rate = fps;
        if (localFile && media_info_.duration <= 0.0 && vstream->duration > 0 && hasValidTimeBase(vstream->time_base)) {
            media_info_.duration = vstream->duration * av_q2d(vstream->time_base);
        }
        if (localFile && media_info_.duration <= 0.0 && vstream->nb_frames > 0 && fps > 0.0) {
            media_info_.duration = static_cast<double>(vstream->nb_frames) / fps;
        }
        media_info_.video_info.codec_name = avcodec_get_name(video_codec_ctx_->codec_id);
        if (const char* pixel_format = av_get_pix_fmt_name(video_codec_ctx_->pix_fmt)) {
            media_info_.video_info.pixel_format = pixel_format;
        }
        media_info_.video_info.bit_rate = video_codec_ctx_->bit_rate;
        media_info_.video_info.duration = media_info_.duration;
        if (localFile) {
            const double frames = media_info_.duration * fps;
            media_info_.video_info.total_frames =
                static_cast<int>(std::min(frames, static_cast<double>(std::numeric_limits<int>::max())));
        }
    }

    // 音频流信息
    if (audio_stream_index_ != -1 && audio_codec_ctx_) {
        media_info_.audio_info.sample_rate = audio_codec_ctx_->sample_rate;
        media_info_.audio_info.channels = audio_codec_ctx_->ch_layout.nb_channels;
        media_info_.audio_info.codec_name = avcodec_get_name(audio_codec_ctx_->codec_id);
        if (const char* sample_format = av_get_sample_fmt_name(audio_codec_ctx_->sample_fmt)) {
            media_info_.audio_info.sample_format = sample_format;
        }
        media_info_.audio_info.bit_rate = audio_codec_ctx_->bit_rate;
        media_info_.audio_info.duration = media_info_.duration;
    }
}
