/**
 * @file PlayerController.cpp
 * @brief 播放控制器实现 - Qt版本
 * 
 * Qt版本改动：
 * - 视频帧通过VideoWidget::setVideoFrame()传递（线程安全）
 * - 频谱数据通过VideoWidget::setSpectrumData()传递
 * - 不再直接调用OpenGL渲染（由VideoWidget在Qt线程中处理）
 * - 解码线程只负责解码和数据传递
 */

#include "PlayerController.h"
#include "VideoWidget.h"
#include "AudioVisualizer.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <utility>

PlayerController::PlayerController()
    : video_widget_(nullptr)
    , playing_(false)
    , paused_(false)
    , stop_requested_(false)
    , session_id_(0)
    , current_time_(0.0)
    , duration_(0.0)
    , volume_(1.0f)
    , speed_(1.0f)
    , seek_requested_(false)
    , pending_seek_seconds_(0.0)
    , render_fps_(0.0)
    , decode_fps_(0.0)
    , av_diff_ms_(0.0)
    , dropped_frames_total_(0)
    , current_effect_(VideoEffect::None)
    , show_visualizer_(false)
{
    decoder_ = std::make_unique<MediaDecoder>();
    audio_output_ = std::make_unique<AudioOutput>();
}

PlayerController::~PlayerController()
{
    stop();
}

// ============================================================
// 加载文件
// ============================================================
// 本地入口沿用现有加载线程；为本次尝试分配独立的取消令牌并发布元信息副本。
bool PlayerController::loadFile(const std::string& filename)
{
    std::uint64_t observed_stop_epoch;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        observed_stop_epoch = stop_epoch_;
    }
    const auto source = PlaybackSource::localFile(filename);
    const auto io = std::make_shared<MediaIoControl>();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);

    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        // 等待上一加载/收尾期间若已收到停止，不再开启这条旧请求。
        if (observed_stop_epoch != stop_epoch_) return false;
        requestStopLocked();
        active_io_ = io;
        ++snapshot_.request_id;
        snapshot_.connection_attempt_id = 1;  // 当前没有自动重连，每个请求只进行首次连接。
        snapshot_.source_kind = source.kind();
        snapshot_.display_name = source.displayName();
        snapshot_.capabilities = initialPlaybackCapabilities(source.kind(), false);
        snapshot_.state = PlaybackState::Opening;
        snapshot_.media_info = {};
        snapshot_.last_error.reset();
        snapshot_.network_stats.reset();
        duration_ = 0.0;
    }

    // 停止与新令牌的发布已串行完成；等待和 FFmpeg 调用不占用状态锁。
    audio_output_->reset();
    if (playback_thread_.joinable()) playback_thread_.join();

    const MediaOpenResult open_result = decoder_->open(source, io);
    if (open_result.status != MediaOpenStatus::Opened) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (io->isCancelled() || open_result.status == MediaOpenStatus::Cancelled) {
            snapshot_.state = PlaybackState::Idle;
        }
        else {
            snapshot_.state = PlaybackState::Failed;
            snapshot_.last_error = open_result;
        }
        return false;
    }

    if (!io->isCancelled() && !decoder_->setPlaybackSpeed(speed_.load())) {
        std::cerr << "[Player] 初始化播放速度失败" << std::endl;
    }

    MediaInfo info = decoder_->getMediaInfo();
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (!io->isCancelled()) {
            duration_ = info.duration;
            current_time_ = 0.0;
            snapshot_.capabilities = initialPlaybackCapabilities(source.kind(), info.duration > 0.0);
            snapshot_.media_info = std::move(info);
            snapshot_.state = PlaybackState::Idle;  // 仍由现有 UI 发起 play()。
            return true;
        }
        snapshot_.state = PlaybackState::Idle;
    }

    // 打开成功与停止恰好交错时，不发布已经取消的结果。
    decoder_->close();
    return false;
}

// ============================================================
// 播放控制
// ============================================================
void PlayerController::play()
{
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    // 停止后重播沿用 loadFile()，不要复用上次已取消的令牌。
    if (!active_io_ || active_io_->isCancelled() || snapshot_.state == PlaybackState::Failed) return;
    if (playing_ && !paused_) return;

    if (paused_) {
        paused_ = false;
        snapshot_.state = resume_state_;
        return;
    }
    if (playback_thread_.joinable()) return;  // 旧线程由 loadFile() 或 stop() 回收。

    playing_ = true;
    paused_ = false;
    stop_requested_ = false;
    audio_packets_.reset();
    video_packets_.reset();
    ++session_id_;   // 新session，旧线程会因sid不匹配而退出

    // 保持 V1 启动语义；网络首个有效输出的判据在网络播放接入时实现。
    snapshot_.state = PlaybackState::Playing;
    playback_thread_ = std::thread(&PlayerController::playbackThread, this, snapshot_.request_id);
}

void PlayerController::pause()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (!playing_ || paused_) return;
    resume_state_ = snapshot_.state;
    snapshot_.state = PlaybackState::Paused;
    paused_ = true;
}

// snapshot_mutex_ 保护取消与队列清理的发布，避免停止清掉刚启动的新会话队列。
void PlayerController::requestStopLocked()
{
    if (active_io_) active_io_->cancel();
    stop_requested_ = true;
    playing_ = false;
    paused_ = false;
    seek_requested_ = false;
    current_time_ = 0.0;
    audio_buffer_seconds_ = 0.0;
    audio_packets_.abort();
    video_packets_.abort();
}

// 先发布取消，独立于正在打开文件或读包的生命周期锁与解码锁。
void PlayerController::stopAsync()
{
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        const bool needs_unwind = playing_ || paused_ || snapshot_.state == PlaybackState::Opening ||
            snapshot_.state == PlaybackState::Stopping;
        ++stop_epoch_;
        requestStopLocked();
        snapshot_.state = needs_unwind ? PlaybackState::Stopping : PlaybackState::Idle;
    }
    // 保留 V1 的设备重置方式；声卡操作的线程归属在 1.3 收束。
    audio_output_->reset();
}

void PlayerController::stop()
{
    // 在等待生命周期锁之前取消打开/探测，否则持锁打开时可能无法及时退出。
    stopAsync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);

    bool newer_attempt = false;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        newer_attempt = active_io_ && !active_io_->isCancelled();
    }
    if (newer_attempt) stopAsync();
    if (playback_thread_.joinable()) playback_thread_.join();
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (snapshot_.state == PlaybackState::Stopping) snapshot_.state = PlaybackState::Idle;
}

void PlayerController::seek(double seconds)
{
    seconds = std::max(0.0, std::min(seconds, duration_.load()));
    pending_seek_seconds_ = seconds;
    seek_requested_ = true;
    current_time_ = seconds;
}

void PlayerController::setVolume(float volume)
{
    volume_ = std::max(0.0f, std::min(1.0f, volume));
}

void PlayerController::setSpeed(float speed)
{
    speed_ = std::max(0.25f, std::min(4.0f, speed));
}

void PlayerController::setEffect(VideoEffect effect)
{
    current_effect_ = effect;
}

PlaybackStats PlayerController::getPlaybackStats() const
{
    PlaybackStats stats;
    stats.render_fps = render_fps_.load();
    stats.decode_fps = decode_fps_.load();
    stats.av_diff_ms = av_diff_ms_.load();
    stats.dropped_frames_total = dropped_frames_total_.load();
    stats.audio_queue_packets = audio_packets_.size();
    stats.video_queue_packets = video_packets_.size();
    stats.audio_buffer_seconds = audio_buffer_seconds_.load();
    return stats;
}

// 按值复制状态和元信息；快速变化的统计由原子缓存/队列计数采样。
PlaybackSnapshot PlayerController::getPlaybackSnapshot() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    PlaybackSnapshot snapshot = snapshot_;
    snapshot.is_playing = playing_.load();
    snapshot.is_paused = paused_.load();
    snapshot.current_seconds = current_time_.load();
    snapshot.duration_seconds = duration_.load();
    snapshot.stats = getPlaybackStats();
    return snapshot;
}

// 数据库查询在调用者线程完成，只把仍属于当前本地请求的字段写回快照。
bool PlayerController::updateLocalLibraryInfo(std::uint64_t expected_request_id, const MediaInfo& info)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (expected_request_id == 0 || expected_request_id != snapshot_.request_id ||
        snapshot_.source_kind != PlaybackSourceKind::LocalFile || snapshot_.media_info.file_path.empty() ||
        snapshot_.media_info.file_path != info.file_path) {
        return false;
    }

    auto& current = snapshot_.media_info;
    current.db_id = info.db_id;
    current.title = info.title;
    current.tags = info.tags;
    current.is_favorite = info.is_favorite;
    current.added_time = info.added_time;
    current.play_count = info.play_count;
    current.last_play_time = info.last_play_time;
    return true;
}

void PlayerController::togglePlayPause()
{
    if (playing_ && !paused_) {
        pause();
    }
    else {
        play();
    }
}

// ============================================================
// 解复用与控制循环，协调独立的音频和视频工作线程。
// ============================================================
void PlayerController::playbackThread(std::uint64_t request_id)
{
    render_fps_.store(0.0);
    decode_fps_.store(0.0);
    av_diff_ms_.store(0.0);
    dropped_frames_total_.store(0);
    audio_buffer_seconds_.store(0.0);

    bool audio_ready = false;
    if (decoder_->hasAudio()) {
        audio_ready = audio_output_->init(decoder_->getOutputAudioSampleRate(), decoder_->getOutputAudioChannels());
        if (!audio_ready) {
            std::cerr << "[Player] 音频输出初始化失败" << std::endl;
        }
        else {
            std::cout << "[Player] 音频输出已就绪: "
                      << decoder_->getOutputAudioSampleRate() << "Hz, "
                      << decoder_->getOutputAudioChannels() << "ch" << std::endl;
        }
    }

    const int my_session = session_id_.load();
    const bool use_audio_queue = decoder_->hasAudio() && audio_ready;
    const bool use_video_queue = decoder_->hasVideo();
    std::atomic<float> active_speed(std::max(0.25f, speed_.load()));
    std::atomic<bool> speed_change_pending(false);
    std::atomic<bool> audio_clock_active(false);
    std::atomic<double> audio_clock_base(current_time_.load());
    std::atomic<double> min_valid_pts(-1.0);
    std::atomic<int> stream_generation(0);

    // 复用现有工作线程采样声卡缓冲。读包结束后的同步等待也能继续更新，GUI 只读原子缓存。
    using StatsClock = std::chrono::steady_clock;
    const auto stats_interval = std::chrono::duration_cast<StatsClock::duration>(std::chrono::milliseconds(100)).count();
    std::atomic<StatsClock::rep> next_stats_sample(StatsClock::now().time_since_epoch().count());
    auto sample_audio_buffer = [&]() {
        if (!audio_ready) return;
        const auto now = StatsClock::now().time_since_epoch().count();
        auto next = next_stats_sample.load();
        if (now >= next && next_stats_sample.compare_exchange_strong(next, now + stats_interval)) {
            audio_buffer_seconds_.store(audio_output_->getQueuedSeconds());
        }
    };

    auto get_audio_clock = [&]() -> double {
        if (!audio_ready || !audio_clock_active.load()) return current_time_.load();
        sample_audio_buffer();
        return audio_clock_base.load() + audio_output_->getPlayedSeconds() * active_speed.load();
    };

    auto is_running = [&]() -> bool {
        return !stop_requested_ && playing_ && session_id_.load() == my_session;
    };

    auto release_frame = [](DecodedFrame& frame) {
        if (frame.data) {
            av_freep(&frame.data);
            frame.data = nullptr;
        }
    };

    std::thread audio_thread;
    if (use_audio_queue) {
        audio_thread = std::thread([&, my_session]() {
            AudioVisualizer fft_calculator;
            fft_calculator.init(64, false);
            bool audio_paused = false;
            AVPacket* packet = av_packet_alloc();
            if (!packet) return;

            auto process_audio_frame = [&](DecodedFrame& frame, int frame_generation) {
                if (!frame.data || frame.data_size <= 0) return;

                const int sample_rate = decoder_->getOutputAudioSampleRate();
                const bool can_trim_by_pts = std::fabs(active_speed.load() - 1.0f) < 0.001f;
                const double frame_end_pts = frame.pts + static_cast<double>(frame.samples) / sample_rate;
                const double target_pts = min_valid_pts.load();
                if (can_trim_by_pts && target_pts >= 0.0 && frame_end_pts <= target_pts) {
                    return;
                }

                int trim_bytes = 0;
                double effective_pts = frame.pts;
                if (can_trim_by_pts && target_pts >= 0.0 && frame.pts < target_pts && frame_end_pts > target_pts) {
                    const int bytes_per_sample_frame = frame.samples > 0 ? frame.data_size / frame.samples : 0;
                    const int trim_samples = std::min(frame.samples,
                        static_cast<int>(std::ceil((target_pts - frame.pts) * sample_rate)));
                    trim_bytes = trim_samples * bytes_per_sample_frame;
                    effective_pts = target_pts;
                }

                if (trim_bytes >= frame.data_size) return;

                std::vector<uint8_t> volume_data(frame.data + trim_bytes, frame.data + frame.data_size);
                float current_volume = volume_.load();
                if (current_volume < 1.0f) {
                    int16_t* pcm = (int16_t*)volume_data.data();
                    int sample_count = static_cast<int>(volume_data.size() / 2);
                    for (int i = 0; i < sample_count; i++) {
                        pcm[i] = (int16_t)(pcm[i] * current_volume);
                    }
                }

                bool audio_written = false;
                if (audio_output_->play(volume_data)) {
                    audio_written = true;
                }
                else if (is_running()) {
                    std::cerr << "[Player] 音频写入失败: " << frame.data_size << " bytes" << std::endl;
                }

                if (video_widget_ && show_visualizer_) {
                    fft_calculator.processAudioData(volume_data.data(), static_cast<int>(volume_data.size()),
                        decoder_->getOutputAudioChannels(), decoder_->getOutputAudioSampleRate());
                    video_widget_->setSpectrumData(fft_calculator.getSpectrumData());
                }

                if (audio_written && frame_generation == stream_generation.load()) {
                    if (!audio_clock_active.load()) {
                        audio_clock_base.store(effective_pts);
                        audio_clock_active.store(true);
                    }
                    current_time_ = get_audio_clock();
                }
            };

            while (is_running()) {
                if (paused_) {
                    if (!audio_paused) {
                        audio_output_->pause();
                        audio_paused = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                if (audio_paused) {
                    audio_output_->resume();
                    audio_paused = false;
                }

                // 先让声卡里已经提交的旧速率音频自然排空，再切到新速率。
                if (speed_change_pending.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }

                if (!audio_packets_.pop(packet, true)) {
                    if (audio_packets_.isFinished() && !audio_packets_.isAborted()) {
                        std::vector<DecodedFrame> frames;
                        {
                            std::lock_guard<std::mutex> lock(seek_mutex_);
                            decoder_->flushAudioDecoder(frames);
                        }
                        int frame_generation = stream_generation.load();
                        for (auto& frame : frames) {
                            if (!is_running() || frame_generation != stream_generation.load()) {
                                release_frame(frame);
                                continue;
                            }
                            process_audio_frame(frame, frame_generation);
                            release_frame(frame);
                        }
                    }
                    break;
                }

                int packet_generation = stream_generation.load();
                std::vector<DecodedFrame> frames;
                {
                    std::lock_guard<std::mutex> lock(seek_mutex_);
                    if (packet_generation == stream_generation.load() && !seek_requested_) {
                        decoder_->decodeAudioPacket(packet, frames);
                    }
                }
                av_packet_unref(packet);

                if (packet_generation != stream_generation.load() || seek_requested_) {
                    for (auto& frame : frames) release_frame(frame);
                    continue;
                }

                for (auto& frame : frames) {
                    if (!is_running() || packet_generation != stream_generation.load()) {
                        release_frame(frame);
                        continue;
                    }
                    process_audio_frame(frame, packet_generation);
                    release_frame(frame);
                }
            }

            av_packet_free(&packet);
        });
    }

    std::thread video_thread;
    if (use_video_queue) {
        video_thread = std::thread([&, my_session]() {
            AVPacket* packet = av_packet_alloc();
            if (!packet) return;

            auto playback_anchor = std::chrono::steady_clock::now();
            double anchor_pts = current_time_.load();
            int local_generation = stream_generation.load();
            auto stats_window_start = std::chrono::steady_clock::now();
            int decoded_frames_window = 0;
            int rendered_frames_window = 0;

            auto reset_video_anchor = [&]() {
                playback_anchor = std::chrono::steady_clock::now();
                anchor_pts = current_time_.load();
                local_generation = stream_generation.load();
            };

            auto wait_until_time = [&](std::chrono::steady_clock::time_point target_time) {
                while (is_running() && !paused_ && std::chrono::steady_clock::now() < target_time) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            };

            auto process_video_frame = [&](DecodedFrame& frame, int frame_generation) {
                if (frame_generation != stream_generation.load() || seek_requested_) {
                    return;
                }

                bool display_frame = true;

                if (audio_ready && audio_clock_active.load()) {
                    constexpr double sync_threshold = 0.04;
                    constexpr double drop_threshold = 0.12;

                    double diff = frame.pts - get_audio_clock();
                    av_diff_ms_.store(diff * 1000.0);
                    while (is_running() && !paused_ &&
                        frame_generation == stream_generation.load() && !seek_requested_ &&
                        diff > sync_threshold) {
                        double wait_seconds = std::min(diff, 0.01);
                        std::this_thread::sleep_for(std::chrono::duration<double>(wait_seconds));
                        diff = frame.pts - get_audio_clock();
                    }

                    if (frame_generation != stream_generation.load() || seek_requested_) {
                        return;
                    }

                    if (diff < -drop_threshold) {
                        display_frame = false;
                        dropped_frames_total_.fetch_add(1);
                    }
                }
                else if (!audio_ready) {
                    if (local_generation != stream_generation.load()) {
                        reset_video_anchor();
                    }

                    double target_delay = (frame.pts - anchor_pts) / active_speed.load();
                    if (target_delay > 0.0) {
                        auto target_time = playback_anchor + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(target_delay));
                        wait_until_time(target_time);
                    }
                }

                if (display_frame && frame.data && video_widget_ && is_running() && !paused_) {
                    const uint8_t* planes[3] = { frame.planes[0], frame.planes[1], frame.planes[2] };
                    video_widget_->setVideoFrame(frame.video_format, planes, frame.linesizes,
                        frame.video_full_range, frame.video_bt709, frame.width, frame.height);
                    ++rendered_frames_window;
                }
                current_time_ = (audio_ready && audio_clock_active.load()) ? get_audio_clock() : frame.pts;

                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - stats_window_start).count();
                if (elapsed >= 1.0) {
                    render_fps_.store(rendered_frames_window / elapsed);
                    decode_fps_.store(decoded_frames_window / elapsed);
                    rendered_frames_window = 0;
                    decoded_frames_window = 0;
                    stats_window_start = now;
                }
            };

            while (is_running()) {
                if (paused_) {
                    reset_video_anchor();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                if (local_generation != stream_generation.load()) {
                    reset_video_anchor();
                }

                if (!video_packets_.pop(packet, true)) {
                    if (video_packets_.isFinished() && !video_packets_.isAborted()) {
                        std::vector<DecodedFrame> frames;
                        {
                            std::lock_guard<std::mutex> lock(seek_mutex_);
                            decoder_->flushVideoDecoder(frames);
                        }
                        int frame_generation = stream_generation.load();
                        for (auto& frame : frames) {
                            if (!is_running() || frame_generation != stream_generation.load()) {
                                release_frame(frame);
                                continue;
                            }
                            process_video_frame(frame, frame_generation);
                            release_frame(frame);
                        }
                    }
                    break;
                }

                int packet_generation = stream_generation.load();
                std::vector<DecodedFrame> frames;
                {
                    std::lock_guard<std::mutex> lock(seek_mutex_);
                    if (packet_generation == stream_generation.load() && !seek_requested_) {
                        decoder_->decodeVideoPacket(packet, frames);
                    }
                }
                av_packet_unref(packet);
                decoded_frames_window += static_cast<int>(frames.size());

                if (packet_generation != stream_generation.load() || seek_requested_) {
                    for (auto& frame : frames) release_frame(frame);
                    continue;
                }

                for (auto& frame : frames) {
                    if (!is_running() || packet_generation != stream_generation.load()) {
                        release_frame(frame);
                        continue;
                    }
                    process_video_frame(frame, packet_generation);
                    release_frame(frame);
                }
            }

            av_packet_free(&packet);
        });
    }

    AVPacket* packet = av_packet_alloc();
    std::optional<MediaReadResult> terminal_read;
    if (!packet) terminal_read = MediaReadResult{MediaReadStatus::Error, AVERROR(ENOMEM)};
    while (packet && is_running()) {
        sample_audio_buffer();
        if (seek_requested_) {
            double seek_seconds = pending_seek_seconds_.load();
            stream_generation.fetch_add(1);
            audio_packets_.reset();
            video_packets_.reset();
            audio_clock_active.store(false);
            min_valid_pts.store(seek_seconds);
            if (audio_ready) {
                audio_output_->reset();
            }
            {
                std::lock_guard<std::mutex> lock(seek_mutex_);
                decoder_->seek(seek_seconds);
                decoder_->setPlaybackSpeed(active_speed.load());
            }
            current_time_ = seek_seconds;
            seek_requested_ = std::fabs(pending_seek_seconds_.load() - seek_seconds) > 0.0001;
            continue;
        }

        float requested_speed = std::max(0.25f, speed_.load());
        if (std::fabs(requested_speed - active_speed.load()) > 0.001f) {
            speed_change_pending.store(true);

            constexpr double speed_switch_queue_threshold = 0.05;
            if (audio_ready && audio_output_->getQueuedSeconds() > speed_switch_queue_threshold) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            double sync_time = audio_clock_active.load() ? get_audio_clock() : current_time_.load();
            stream_generation.fetch_add(1);
            audio_clock_active.store(false);
            if (audio_ready) {
                audio_output_->reset();
            }
            {
                std::lock_guard<std::mutex> lock(seek_mutex_);
                if (!decoder_->setPlaybackSpeed(requested_speed)) {
                    std::cerr << "[Player] 更新播放速度失败: " << requested_speed << "x" << std::endl;
                }
            }
            active_speed.store(requested_speed);
            speed_change_pending.store(false);
            current_time_ = sync_time;
            continue;
        }
        else if (speed_change_pending.load()) {
            speed_change_pending.store(false);
        }

        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const bool audio_queue_full = !use_audio_queue || audio_packets_.size() > 80;
        const bool video_queue_full = !use_video_queue || video_packets_.size() > 80;
        if (audio_queue_full && video_queue_full) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        MediaReadResult read_result;
        bool is_audio_packet = false;
        bool is_video_packet = false;
        {
            std::lock_guard<std::mutex> lock(seek_mutex_);
            read_result = decoder_->readPacketResult(packet);
            if (read_result.status == MediaReadStatus::Packet) {
                is_audio_packet = decoder_->isAudioPacket(packet);
                is_video_packet = decoder_->isVideoPacket(packet);
            }
        }

        if (read_result.status == MediaReadStatus::Again) {
            // 暂无数据不等于文件结束，短暂让出线程后重新检查停止标志。
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (read_result.status != MediaReadStatus::Packet) {
            terminal_read = read_result;
            break;
        }

        if (is_audio_packet && use_audio_queue) {
            audio_packets_.push(packet);
        }
        else if (is_video_packet && use_video_queue) {
            video_packets_.push(packet);
        }
        av_packet_unref(packet);
    }

    if (packet) {
        av_packet_free(&packet);
    }

    const bool read_cancelled = terminal_read && terminal_read->status == MediaReadStatus::Cancelled;
    const bool read_failed = terminal_read && (terminal_read->status == MediaReadStatus::Timeout ||
        terminal_read->status == MediaReadStatus::Error);
    if (stop_requested_ || session_id_.load() != my_session || read_cancelled) {
        audio_packets_.abort();
        video_packets_.abort();
    }
    else {
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            if (snapshot_.request_id == request_id && !stop_requested_) {
                if (paused_) resume_state_ = PlaybackState::Draining;
                else snapshot_.state = PlaybackState::Draining;
                if (read_failed) snapshot_.last_error = *terminal_read;
            }
        }
        // 保留本地已有的队列排空逻辑；本项只区分结束原因和对外状态。
        audio_packets_.finish();
        video_packets_.finish();
    }

    if (audio_thread.joinable()) audio_thread.join();
    if (video_thread.joinable()) video_thread.join();

    if (audio_ready) {
        audio_output_->cleanup();
    }
    audio_buffer_seconds_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        // 切源可能已发布新的 Opening，旧播放结束不得覆盖新请求的状态或错误。
        if (snapshot_.request_id != request_id) return;
        playing_ = false;
        paused_ = false;
        if (stop_requested_ || read_cancelled) {
            snapshot_.state = PlaybackState::Idle;
            current_time_ = 0.0;
        }
        else {
            snapshot_.state = read_failed ? PlaybackState::Failed : PlaybackState::Ended;
        }
    }
}
