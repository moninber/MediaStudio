#include "PlayerController.h"
#include "VideoWidget.h"
#include "AudioVisualizer.h"
#include "AudioOutput.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <utility>

using namespace std::chrono_literals;

PlayerController::PlayerController()
{
    supervisor_ = std::thread(&PlayerController::supervisorLoop, this);
}

PlayerController::~PlayerController()
{
    shutdown();
}

void PlayerController::setVideoWidget(VideoWidget* widget)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    video_widget_ = widget;
}

PlaybackEpoch PlayerController::currentEpochLocked() const
{
    return {snapshot_.request_id, snapshot_.connection_attempt_id, snapshot_.generation};
}

void PlayerController::resetSnapshotLocked(std::uint64_t request_id, PlaybackState state)
{
    snapshot_ = {};
    snapshot_.request_id = request_id;
    snapshot_.state = state;
    current_time_ = 0.0;
    duration_ = 0.0;
}

void PlayerController::cancelCurrentLocked()
{
    if (active_io_) active_io_->cancel();
    pending_open_.reset();
    pending_seek_.reset();
    seek_requested_ = false;
    playing_ = false;
    paused_ = false;
    audio_packets_.abort();
    video_packets_.abort();
}

std::uint64_t PlayerController::requestOpen(const PlaybackSource& source)
{
    auto io = std::make_shared<MediaIoControl>();
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (shutting_down_) return 0;

    const PlaybackEpoch epoch{snapshot_.request_id + 1, 1, 1};
    cancelCurrentLocked();
    resetSnapshotLocked(epoch.request_id, PlaybackState::Opening);
    snapshot_.connection_attempt_id = epoch.connection_attempt_id;
    snapshot_.generation = epoch.generation;
    snapshot_.source_kind = source.kind();
    snapshot_.display_name = source.displayName();
    snapshot_.capabilities = initialPlaybackCapabilities(source.kind(), false);
    resume_state_ = PlaybackState::Playing;
    if (video_widget_) video_widget_->setPlaybackEpoch(epoch, false);

    // 先发布可取消对象，再唤醒 supervisor。打开尚未开始时也能取消这条命令。
    active_io_ = io;
    pending_open_ = OpenRequest{source, epoch, std::move(io)};
    command_changed_.notify_one();
    return epoch.request_id;
}

std::uint64_t PlayerController::stopAsync()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    const auto request_id = snapshot_.request_id;
    cancelCurrentLocked();
    resetSnapshotLocked(request_id, supervisor_busy_ ? PlaybackState::Stopping : PlaybackState::Idle);
    if (video_widget_) video_widget_->setPlaybackEpoch({}, false);
    return request_id;
}

void PlayerController::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        shutting_down_ = true;
        cancelCurrentLocked();
    }
    command_changed_.notify_one();
    if (supervisor_.joinable()) supervisor_.join();
}

void PlayerController::setPausedLocked(bool paused)
{
    if (!playing_ || paused_ == paused || snapshot_.source_kind != PlaybackSourceKind::LocalFile) return;
    if (paused) {
        resume_state_ = snapshot_.state;
        snapshot_.state = PlaybackState::Paused;
    }
    else {
        snapshot_.state = resume_state_;
    }
    paused_ = paused;
}

void PlayerController::play()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    setPausedLocked(false);
}

void PlayerController::pause()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    setPausedLocked(true);
}

void PlayerController::togglePlayPause()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    setPausedLocked(!paused_.load());
}

void PlayerController::seek(double seconds)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (!playing_ || !snapshot_.capabilities.can_seek || !std::isfinite(seconds)) return;
    pending_seek_ = std::clamp(seconds, 0.0, duration_.load());
    seek_requested_ = true;
}

void PlayerController::setVolume(float volume)
{
    volume_ = std::clamp(volume, 0.0f, 1.0f);
}

void PlayerController::setSpeed(float speed)
{
    if (std::isfinite(speed)) speed_ = std::clamp(speed, 0.25f, 4.0f);
}

void PlayerController::setEffect(VideoEffect effect)
{
    current_effect_ = effect;
}

PlaybackSnapshot PlayerController::getPlaybackSnapshot() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    PlaybackSnapshot snapshot = snapshot_;
    snapshot.is_playing = playing_.load();
    snapshot.is_paused = paused_.load();
    snapshot.current_seconds = current_time_.load();
    snapshot.duration_seconds = duration_.load();
    snapshot.stats.audio_queue_packets = audio_packets_.size();
    snapshot.stats.video_queue_packets = video_packets_.size();
    return snapshot;
}

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

// 一个受管理线程拥有解复用资源。取消旧连接后，必须先回收它的 worker 才能复用队列。
void PlayerController::supervisorLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        command_changed_.wait(lock, [&] { return shutting_down_ || pending_open_.has_value(); });
        if (shutting_down_) return;
        auto request = std::move(*pending_open_);
        pending_open_.reset();
        auto* video = video_widget_;
        supervisor_busy_ = true;
        lock.unlock();

        const auto error = runRequest(request, video);
        decoder_.close();

        lock.lock();
        supervisor_busy_ = false;
        if (snapshot_.state == PlaybackState::Stopping) {
            snapshot_.state = PlaybackState::Idle;
        }
        // 切源/停止已经发布新的快照，旧请求的错误和收尾不能再修改它。
        if (request.io->isCancelled() || snapshot_.request_id != request.epoch.request_id ||
            snapshot_.connection_attempt_id != request.epoch.connection_attempt_id) continue;
        active_io_.reset();
        playing_ = false;
        paused_ = false;
        pending_seek_.reset();
        seek_requested_ = false;
        snapshot_.stats.audio_buffer_seconds = 0.0;
        snapshot_.state = error ? PlaybackState::Failed : PlaybackState::Ended;
        snapshot_.last_error = error;
    }
}

std::optional<PlaybackError> PlayerController::runRequest(const OpenRequest& request, VideoWidget* video)
{
    const auto result = decoder_.open(request.source, request.io);
    if (result.status != MediaOpenStatus::Opened) return result;

    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (request.io->isCancelled() || currentEpochLocked() != request.epoch) return {};
        snapshot_.media_info = decoder_.getMediaInfo();
        duration_ = snapshot_.media_info.duration;
        snapshot_.capabilities = initialPlaybackCapabilities(request.source.kind(), duration_ > 0.0);
        snapshot_.state = PlaybackState::Playing;
        audio_packets_.reset();
        video_packets_.reset();
        playing_ = true;
    }

    const auto read_error = demuxAndPlay(request, video);
    if (read_error) return *read_error;
    return {};
}

std::optional<MediaReadResult> PlayerController::demuxAndPlay(const OpenRequest& request, VideoWidget* video)
{
    if (request.io->isCancelled()) return {};
    const bool local_file = request.source.kind() == PlaybackSourceKind::LocalFile;
    const bool use_video = decoder_.hasVideo();

    std::atomic<float> active_speed(local_file ? speed_.load() : 1.0f);
    decoder_.setPlaybackSpeed(active_speed.load());
    std::atomic<bool> speed_change_pending(false);
    std::atomic<double> min_valid_pts(-1.0);
    std::atomic<std::uint64_t> stream_generation(request.epoch.generation);
    std::atomic<std::uint64_t> audio_drained(0);
    std::atomic<std::uint64_t> video_drained(0);
    std::atomic<bool> reconfiguring(false);
    std::atomic<bool> workers_done(false);
    std::atomic<bool> worker_allocation_failed(false);
    std::atomic<int> audio_error(0);

    auto epoch_now = [&] {
        return PlaybackEpoch{request.epoch.request_id, request.epoch.connection_attempt_id, stream_generation.load()};
    };
    auto running = [&] { return !request.io->isCancelled() && !workers_done; };
    auto is_current = [&](PlaybackEpoch epoch) {
        return running() && !seek_requested_ && !reconfiguring && epoch == epoch_now();
    };
    auto publish = [&](PlaybackEpoch epoch, const auto& update) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (is_current(epoch) && epoch == currentEpochLocked()) update();
    };

    using Clock = std::chrono::steady_clock;
    struct AudioClockSample {
        PlaybackEpoch epoch;
        std::optional<double> position;
        double queued_seconds = 0.0;
        bool tail_drained = false;
    };
    // 音频线程从声卡采样后整体发布，视频/解复用线程只读取匹配代次的副本。
    AudioClockSample audio_clock;
    auto read_audio_clock = [&](PlaybackEpoch epoch) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        return audio_clock.epoch == epoch ? audio_clock : AudioClockSample{};
    };

    // 成功出队后的帧沿用该包的完整标签。EOF 刷新则使用调用 flush 之前捕获的标签。
    auto consume = [&](PacketQueue& queue, std::atomic<std::uint64_t>& drained,
                       auto decode, auto flush, bool blocking_pop, const auto& ready, const auto& output) {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            worker_allocation_failed = true;
            return;
        }
        while (running() && !audio_error) {
            auto epoch = epoch_now();
            if (!ready(epoch) || !is_current(epoch)) {
                std::this_thread::sleep_for(5ms);
                continue;
            }
            if (drained == epoch.generation) {
                std::this_thread::sleep_for(2ms);
                continue;
            }

            const bool has_packet = queue.pop(packet, epoch, blocking_pop);
            if (!has_packet && (!queue.isFinished() || queue.isAborted() || !running())) {
                if (!blocking_pop) std::this_thread::sleep_for(5ms);
                continue;
            }
            std::vector<DecodedFrame> frames;
            bool flushed = false;
            {
                std::lock_guard<std::mutex> lock(codec_mutex_);
                if (is_current(epoch)) {
                    if (has_packet) (decoder_.*decode)(packet, frames);
                    else {
                        (decoder_.*flush)(frames);
                        flushed = true;
                    }
                }
            }
            av_packet_unref(packet);

            for (auto& frame : frames) {
                if (is_current(epoch)) output(frame, epoch);
                av_freep(&frame.data);
            }
            if (flushed && is_current(epoch)) drained = epoch.generation;
        }
        av_packet_free(&packet);
    };

    std::thread audio_worker;
    bool audio_ready = false;
    if (decoder_.hasAudio()) {
        std::promise<bool> initialized;
        auto audio_init = initialized.get_future();
        audio_worker = std::thread([&, init = std::move(initialized)]() mutable {
            // 声卡对象和 COM 生命周期都在音频线程，退出此作用域即关闭。
            AudioOutput device;
            const int sample_rate = decoder_.getOutputAudioSampleRate();
            const int channels = decoder_.getOutputAudioChannels();
            const bool opened = running() && device.init(sample_rate, channels);
            init.set_value(opened);
            if (!opened) return;

            AudioVisualizer fft;
            fft.init(64, false);
            std::uint64_t device_generation = request.epoch.generation;
            std::optional<double> clock_base;
            auto last_sample_at = Clock::now() - 8ms;
            bool device_paused = false;
            std::optional<double> tail_seconds_left;

            auto fail_audio = [&](int code, const char* message) {
                std::cerr << "[Player] " << message << std::endl;
                audio_error = AVERROR(code);
            };
            auto sample_device = [&](PlaybackEpoch epoch, bool force) {
                const auto now = Clock::now();
                if (!force && now - last_sample_at < 8ms) return;
                const double elapsed = std::chrono::duration<double>(now - last_sample_at).count();
                last_sample_at = now;
                const auto progress = device.sample();
                if (!progress.valid) {
                    fail_audio(EIO, "无法读取声卡进度");
                    return;
                }

                AudioClockSample sample{epoch, {}, progress.queued_seconds, false};
                if (clock_base) sample.position = *clock_base + progress.played_seconds * active_speed.load();
                if (audio_drained == epoch.generation) {
                    sample.tail_drained = progress.queued_seconds <= 0.005;
                    if (!tail_seconds_left) tail_seconds_left = std::min(3.0, progress.queued_seconds + 1.0);
                    else if (!device_paused) *tail_seconds_left -= elapsed;
                    if (!sample.tail_drained && *tail_seconds_left <= 0.0 && is_current(epoch)) {
                        fail_audio(ETIMEDOUT, "等待声卡尾音超时");
                    }
                }
                publish(epoch, [&] {
                    audio_clock = sample;
                    snapshot_.stats.audio_buffer_seconds = sample.queued_seconds;
                    if (sample.position && !paused_) current_time_ = *sample.position;
                });
            };
            auto ready = [&](PlaybackEpoch epoch) {
                if (!running() || audio_error) return false;
                // 看到新代次后，先清除旧 padding，再允许这次的 PCM 写入。
                if (is_current(epoch) && device_generation != epoch.generation) {
                    if (!device.reset()) {
                        fail_audio(EIO, "声卡重置失败");
                        return false;
                    }
                    device_generation = epoch.generation;
                    clock_base.reset();
                    tail_seconds_left.reset();
                    fft.resetSpectrum();
                }

                const bool paused = paused_.load();
                if (paused != device_paused) {
                    const bool updated = paused ? device.pause() : device.resume();
                    if (!updated) {
                        fail_audio(EIO, "无法更新声卡暂停状态");
                        return false;
                    }
                    device_paused = paused;
                }
                if (!is_current(epoch)) return false;
                sample_device(epoch, false);
                return !paused && !speed_change_pending && !audio_error;
            };

            auto output = [&](DecodedFrame& frame, PlaybackEpoch epoch) {
                if (!frame.data || frame.data_size <= 0) return;
                const double target = min_valid_pts.load();
                int trim_bytes = 0;
                double effective_pts = frame.pts;
                if (target >= 0.0 && frame.pts < target && std::fabs(active_speed.load() - 1.0f) < 0.001f) {
                    const int skip = static_cast<int>(std::min<double>(frame.samples,
                        std::ceil((target - frame.pts) * sample_rate)));
                    if (skip >= frame.samples) return;
                    trim_bytes = skip * (frame.data_size / frame.samples);
                    effective_pts = target;
                }
                std::vector<uint8_t> pcm(frame.data + trim_bytes, frame.data + frame.data_size);
                const float volume = volume_.load();
                if (volume < 1.0f) {
                    auto* samples = reinterpret_cast<int16_t*>(pcm.data());
                    for (size_t i = 0; i < pcm.size() / sizeof(int16_t); ++i) {
                        samples[i] = static_cast<int16_t>(samples[i] * volume);
                    }
                }

                // 暂停保留本帧的 PCM 写入偏移。停止、切源或换代使此帧失效。
                std::size_t offset = 0;
                auto last_write = Clock::now();
                const auto can_write = [&] { return is_current(epoch) && !paused_ && !speed_change_pending; };
                while (running() && !audio_error && epoch == epoch_now()) {
                    if (!ready(epoch) || !can_write()) {
                        last_write = Clock::now();
                        std::this_thread::sleep_for(5ms);
                        continue;
                    }
                    const auto previous_offset = offset;
                    const auto result = device.write(pcm, offset, can_write);
                    if (offset > previous_offset) {
                        last_write = Clock::now();
                        if (!clock_base) {
                            clock_base = std::max(effective_pts, target);
                            sample_device(epoch, true);
                        }
                    }
                    if (result == AudioWriteResult::Written) break;
                    if (result == AudioWriteResult::Error) {
                        fail_audio(EIO, "PCM 写入失败");
                        return;
                    }
                    if (result == AudioWriteResult::WouldBlock && Clock::now() - last_write >= 1s) {
                        fail_audio(ETIMEDOUT, "等待声卡缓冲空间超时");
                        return;
                    }
                    if (result != AudioWriteResult::Pending) std::this_thread::sleep_for(5ms);
                }

                if (video && show_visualizer_ && can_write() && !audio_error) {
                    fft.processAudioData(pcm.data(), static_cast<int>(pcm.size()), channels, sample_rate);
                    video->setSpectrumData(epoch, fft.getSpectrumData());
                }
            };

            consume(audio_packets_, audio_drained, &MediaDecoder::decodeAudioPacket,
                &MediaDecoder::flushAudioDecoder, false, ready, output);
        });
        // 只在 supervisor 等待初始化结果。窗口线程仍能发布停止或新请求。
        audio_ready = audio_init.get();
        if (!audio_ready) {
            audio_worker.join();
            if (running()) std::cerr << "[Player] 音频输出初始化失败" << std::endl;
        }
    }
    if (!audio_ready && !use_video) return MediaReadResult{MediaReadStatus::Error, AVERROR(EIO)};

    std::thread video_worker;
    if (use_video) {
        video_worker = std::thread([&] {
            auto playback_anchor = Clock::now();
            auto window_start = playback_anchor;
            double anchor_pts = current_time_.load();
            std::uint64_t anchor_generation = 0;
            int decoded_frames = 0;
            int submitted_frames = 0;
            auto reset_anchor = [&] {
                playback_anchor = Clock::now();
                anchor_pts = current_time_.load();
            };
            auto ready = [&](PlaybackEpoch) {
                if (paused_) reset_anchor();
                return !paused_ && !speed_change_pending;
            };

            auto output = [&](DecodedFrame& frame, PlaybackEpoch epoch) {
                if (anchor_generation != epoch.generation) {
                    reset_anchor();
                    anchor_generation = epoch.generation;
                    decoded_frames = 0;
                    submitted_frames = 0;
                    window_start = playback_anchor;
                }
                if (frame.pts + 0.0001 < min_valid_pts.load()) return;
                ++decoded_frames;
                auto audio = read_audio_clock(epoch);
                const bool sync_to_audio = audio.position.has_value();
                double diff = 0.0;
                if (sync_to_audio) {
                    diff = frame.pts - *audio.position;
                    while (is_current(epoch) && !paused_ && !speed_change_pending && diff > 0.04) {
                        if (audio.tail_drained) break;
                        std::this_thread::sleep_for(5ms);
                        audio = read_audio_clock(epoch);
                        if (!audio.position) break;
                        diff = frame.pts - *audio.position;
                    }
                }
                else if (!audio_ready || local_file) {
                    // 本地源刚刷新时，声卡尚无新时钟，先用视频节奏避免一口气提交预读帧。
                    const double delay = (frame.pts - anchor_pts) / active_speed.load();
                    if (delay > 0.0) {
                        const auto at = playback_anchor + std::chrono::duration_cast<Clock::duration>(
                            std::chrono::duration<double>(delay));
                        while (is_current(epoch) && !paused_ && !speed_change_pending && Clock::now() < at) {
                            std::this_thread::sleep_for(1ms);
                        }
                    }
                }
                if (!is_current(epoch) || paused_ || speed_change_pending) return;

                const bool dropped = sync_to_audio && diff < -0.12;
                bool accepted = false;
                if (!dropped && frame.data && video) {
                    const uint8_t* planes[3] = {frame.planes[0], frame.planes[1], frame.planes[2]};
                    accepted = video->setVideoFrame(epoch, frame.video_format, planes, frame.linesizes,
                        frame.video_full_range, frame.video_bt709, frame.width, frame.height);
                    if (accepted) ++submitted_frames;
                }

                const auto now = Clock::now();
                const double elapsed = std::chrono::duration<double>(now - window_start).count();
                publish(epoch, [&] {
                    if (!paused_ && (audio_clock.epoch != epoch || !audio_clock.position)) {
                        current_time_ = frame.pts;
                    }
                    if (accepted) snapshot_.has_current_video_output = true;
                    if (sync_to_audio) snapshot_.stats.av_diff_ms = diff * 1000.0;
                    if (dropped) ++snapshot_.stats.dropped_frames_total;
                    if (elapsed >= 1.0) {
                        snapshot_.stats.render_fps = submitted_frames / elapsed;
                        snapshot_.stats.decode_fps = decoded_frames / elapsed;
                    }
                });
                if (elapsed >= 1.0) {
                    decoded_frames = 0;
                    submitted_frames = 0;
                    window_start = now;
                }
            };

            consume(video_packets_, video_drained, &MediaDecoder::decodeVideoPacket,
                &MediaDecoder::flushVideoDecoder, true, ready, output);
        });
    }

    // 同一个请求只提升媒体代次。可定位输入从原播放位置重新读包。
    auto refresh = [&](float new_speed, double sync_position, bool reposition) {
        double position = sync_position;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            if (!running()) return false;
            if (pending_seek_) {
                position = *pending_seek_;
                reposition = true;
            }
            pending_seek_.reset();
            reconfiguring = true;
            seek_requested_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(codec_mutex_);
            if (reposition && !decoder_.seek(position)) {
                reconfiguring = false;
                return false;
            }
            if (!reposition) decoder_.flushAfterDiscontinuity(position);
            if (!decoder_.setPlaybackSpeed(new_speed)) {
                std::cerr << "[Player] 更新播放速度失败: " << new_speed << "x" << std::endl;
            }
        }
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            if (!running()) return false;
            ++stream_generation;
            audio_packets_.reset();
            video_packets_.reset();
            snapshot_.generation = stream_generation.load();
            snapshot_.has_current_video_output = false;
            snapshot_.stats = {};
            min_valid_pts = position;
            current_time_ = position;
            resume_state_ = PlaybackState::Playing;
            if (!paused_) snapshot_.state = PlaybackState::Playing;
            if (video) video->setPlaybackEpoch(epoch_now(), true);
            active_speed = new_speed;
            speed_change_pending = false;
            reconfiguring = false;
        }
        return true;
    };

    AVPacket* packet = av_packet_alloc();
    std::optional<MediaReadResult> error;
    if (!packet) error = MediaReadResult{MediaReadStatus::Error, AVERROR(ENOMEM)};
    bool draining = false;
    bool can_reposition_for_speed = duration_ > 0.0;

    while (packet && running()) {
        if (worker_allocation_failed) {
            error = MediaReadResult{MediaReadStatus::Error, AVERROR(ENOMEM)};
            break;
        }
        if (const int code = audio_error.load()) {
            error = MediaReadResult{MediaReadStatus::Error, code};
            break;
        }
        const float requested_speed = local_file ? speed_.load() : 1.0f;
        const bool speed_changed = std::fabs(requested_speed - active_speed.load()) > 0.001f;
        const bool seeking = seek_requested_.load();
        const auto epoch = epoch_now();
        if (seeking || speed_changed) {
            const bool reposition = seeking || can_reposition_for_speed;
            // 无法定位时先消费旧队列，避免倍速操作把媒体退回开头或跳过整批预读包。
            if (!reposition && (audio_packets_.size() > 0 || video_packets_.size() > 0)) {
                speed_change_pending = false;
                std::this_thread::sleep_for(2ms);
                continue;
            }
            speed_change_pending = speed_changed;
            const auto audio = read_audio_clock(epoch);
            if (speed_changed && !seeking && !paused_ && audio.queued_seconds > 0.05) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            const double position = std::max(0.0, audio.position.value_or(current_time_.load()));
            if (refresh(requested_speed, position, reposition)) draining = false;
            else {
                can_reposition_for_speed = false;
                speed_change_pending = false;
            }
            continue;
        }
        speed_change_pending = false;
        if (paused_) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (draining) {
            const bool done = (!audio_ready || read_audio_clock(epoch).tail_drained) &&
                              (!use_video || video_drained == epoch.generation);
            if (done) {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                if (running() && !seek_requested_ && !paused_) {
                    const double duration = duration_.load();
                    if (duration > 0.0 && std::fabs(duration - current_time_.load()) <= 0.05) {
                        current_time_ = duration;
                    }
                    playing_ = false;
                    break;
                }
            }
            std::this_thread::sleep_for(2ms);
            continue;
        }

        const bool audio_queue_full = !audio_ready || audio_packets_.size() > 80;
        const bool video_queue_full = !use_video || video_packets_.size() > 80;
        if (audio_queue_full && video_queue_full) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        // 格式上下文仅 supervisor 使用。网络等包期间，worker 仍可用缓存的 time_base 解码。
        const MediaReadResult result = decoder_.readPacketResult(packet);
        if (result.status == MediaReadStatus::Again) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (!running()) break;
        if (result.status == MediaReadStatus::Packet) {
            // 读包前捕获标签，入队时保存。暂停可保留队列，seek 则丢弃这次旧读取。
            if (!seek_requested_) {
                if (audio_ready && decoder_.isAudioPacket(packet)) audio_packets_.push(packet, epoch);
                else if (use_video && decoder_.isVideoPacket(packet)) video_packets_.push(packet, epoch);
            }
            av_packet_unref(packet);
        }
        else if (result.status == MediaReadStatus::EndOfStream && local_file) {
            draining = true;
            resume_state_ = PlaybackState::Draining;
            if (!paused_) snapshot_.state = PlaybackState::Draining;
            audio_packets_.finish();
            video_packets_.finish();
        }
        else {
            error = result;
            break;
        }
    }

    workers_done = true;
    audio_packets_.abort();
    video_packets_.abort();
    av_packet_free(&packet);
    if (audio_worker.joinable()) audio_worker.join();
    if (video_worker.joinable()) video_worker.join();
    return error;
}
