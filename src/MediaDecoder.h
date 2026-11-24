/**
 * @file MediaDecoder.h
 * @brief 媒体解码器 - 基于FFmpeg的音视频解封装与解码
 * 
 * 本类封装了FFmpeg的解封装（Demux）和解码（Decode）流程：
 * 
 * FFmpeg处理流程：
 * 1. avformat_open_input()   → 打开媒体文件，创建AVFormatContext
 * 2. avformat_find_stream_info() → 探测流信息（编码格式、分辨率等）
 * 3. avcodec_find_decoder()  → 查找对应的解码器
 * 4. avcodec_alloc_context3() → 创建解码器上下文
 * 5. avcodec_open2()         → 打开解码器
 * 6. av_read_frame()         → 从文件中读取一个AVPacket（压缩数据包）
 * 7. avcodec_send_packet()   → 将压缩数据发送给解码器
 * 8. avcodec_receive_frame() → 从解码器获取解码后的AVFrame（原始数据）
 * 
 * 关键概念：
 * - AVFormatContext：封装格式上下文，管理文件/流的打开和读取
 * - AVCodecContext：编解码器上下文，管理编解码参数和状态
 * - AVPacket：压缩数据包（一个视频帧的压缩数据或一段音频的压缩数据）
 * - AVFrame：解码后的原始数据（视频=像素数据，音频=PCM采样数据）
 * - SwsContext：图像转换上下文（用于色彩空间转换，如YUV→RGB）
 */

#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <vector>
#include "MediaInfo.h"
#include "MediaIo.h"
#include "PlaybackSource.h"

// FFmpeg头文件使用extern "C"包裹，因为FFmpeg是C库
// C++编译器会对函数名进行name mangling（名称改编），导致链接失败
extern "C" {
#include <libavcodec/avcodec.h>       // 编解码器API
#include <libavformat/avformat.h>     // 封装格式API（解封装/封装）
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>       // 图像工具函数（av_image_alloc等）
#include <libswscale/swscale.h>       // 图像缩放和色彩空间转换
#include <libswresample/swresample.h>  // 音频重采样
#include <libavutil/opt.h>            // 选项设置API
}

struct DecodedFrame {
    enum class Type {
        None,
        Video,
        Audio
    };

    Type type = Type::None;
    VideoFrameFormat video_format = VideoFrameFormat::RGBA;
    uint8_t* data = nullptr;
    uint8_t* planes[3] = { nullptr, nullptr, nullptr };
    int linesizes[3] = { 0, 0, 0 };
    bool video_full_range = false;
    bool video_bt709 = false;
    int data_size = 0;
    int width = 0;
    int height = 0;
    int samples = 0;
    double pts = 0.0;
    bool has_valid_pts = false; // pts 来自解码器时间戳，而非缺失时的回退值。
    bool corrupt = false;      // FFmpeg 明确标记损坏；直播只传回标记，不复制数据。
};

class MediaDecoder {
public:
    MediaDecoder();
    ~MediaDecoder();

    /**
     * @brief 打开媒体文件并初始化解码器
     * @param filename 媒体文件路径
     * @return true=成功, false=失败
     * 
     * 完整流程：打开文件 → 探测流信息 → 查找视频/音频流 → 打开解码器
     */
    bool open(const std::string& filename);
    // 打开显式类型的媒体；取消对象覆盖打开、探测、读取和关闭的完整尝试。
    MediaOpenResult open(const PlaybackSource& source, std::shared_ptr<MediaIoControl> io = {});

    /** @brief 关闭文件并释放所有FFmpeg资源 */
    void close();

    bool readPacket(AVPacket* packet);
    // 只交付选中的音视频包，并区分暂时无包、EOF、超时、取消和错误。
    MediaReadResult readPacketResult(AVPacket* packet);
    bool isVideoPacket(const AVPacket* packet) const;
    bool isAudioPacket(const AVPacket* packet) const;
    bool decodeVideoPacket(const AVPacket* packet, std::vector<DecodedFrame>& out_frames);
    bool decodeAudioPacket(const AVPacket* packet, std::vector<DecodedFrame>& out_frames);
    bool flushVideoDecoder(std::vector<DecodedFrame>& out_frames);
    bool flushAudioDecoder(std::vector<DecodedFrame>& out_frames);

    /**
     * @brief 跳转到指定时间位置
     * @param seconds 目标时间（秒）
     * 
     * 本地文件通过 av_seek_frame() 查找关键帧。网络直播不允许定位。
     */
    bool seek(double seconds);

    // 从给定时间重新建立解码和输出状态，不改变解封装读取位置。
    void flushAfterDiscontinuity(double seconds);

    // 只允许本地文件变速，网络直播只接受原速。
    bool setPlaybackSpeed(float speed);

    /** @brief 获取媒体文件信息（用于显示和数据库存储） */
    const MediaInfo& getMediaInfo() const { return media_info_; }

    // 便捷访问器
    bool hasVideo() const { return media_info_.has_video; }
    bool hasAudio() const { return media_info_.has_audio; }
    // 返回会话打开时按值缓存的 time_base，不在队列统计中跨线程读取 AVStream。
    AVRational audioPacketTimeBase() const noexcept { return audio_time_base_; }
    AVRational videoPacketTimeBase() const noexcept { return video_time_base_; }
    int getOutputAudioSampleRate() const { return TARGET_SAMPLE_RATE; }
    int getOutputAudioChannels() const { return TARGET_CHANNELS; }

private:
    /**
     * @brief 从AVFormatContext中提取媒体信息
     * 填充MediaInfo结构体的各个字段
     */
    void extractMediaInfo(bool localFile);
    bool convertVideoFrame(AVFrame* frame, DecodedFrame& out_frame);
    bool convertAudioFrame(AVFrame* frame, DecodedFrame& out_frame);
    bool pullAudioFilterFrame(DecodedFrame& out_frame);
    void receiveVideoFrames(std::vector<DecodedFrame>& out_frames);
    void receiveAudioFrames(std::vector<DecodedFrame>& out_frames);
    bool initAudioFilterGraph();
    void destroyAudioFilterGraph();
    std::string buildAtempoFilterChain(float speed) const;

    AVFormatContext* format_ctx_;     // 由 supervisor 独占，解码线程不访问 AVStream。
    std::shared_ptr<MediaIoControl> io_; // FFmpeg 回调使用的对象，在关闭封装后释放。
    std::chrono::milliseconds read_timeout_{0};
    bool network_input_ = false;
    bool read_deadline_active_ = false; // Again 和跳过非选中包沿用同一次读取的期限。
    AVCodecContext* video_codec_ctx_; // 视频解码器上下文
    AVCodecContext* audio_codec_ctx_; // 音频解码器上下文
    SwsContext* sws_ctx_;            // 图像转换上下文（YUV→RGB）
    SwrContext* swr_ctx_;            // 音频重采样上下文
    AVFilterGraph* audio_filter_graph_;
    AVFilterContext* audio_buffer_src_ctx_;
    AVFilterContext* audio_buffer_sink_ctx_;

    int video_stream_index_;         // 视频流索引（-1=无视频流）
    int audio_stream_index_;         // 音频流索引（-1=无音频流）
    // 打开时按值复制，整个解码会话只读，等解码线程退出后再清除。
    AVRational video_time_base_{0, 1};
    AVRational audio_time_base_{0, 1};

    std::atomic<double> current_time_;            // 当前播放时间（秒）
    float playback_speed_;
    MediaInfo media_info_;           // 媒体文件信息
    bool audio_drained_;
    bool audio_filter_flush_sent_;
    int64_t min_audio_input_pts_ = AV_NOPTS_VALUE;  // atempo 输入的最早采样位置，单位 1/44100 秒。
    double last_video_pts_;
    bool has_last_video_pts_;

    // 音频重采样目标参数
    static constexpr int TARGET_SAMPLE_RATE = 44100;  // 目标采样率
    static constexpr int TARGET_CHANNELS = 2;         // 目标声道数（立体声）
    static constexpr AVSampleFormat TARGET_SAMPLE_FMT = AV_SAMPLE_FMT_S16; // 16位PCM
};
