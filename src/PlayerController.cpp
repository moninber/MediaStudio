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

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto LiveInitialOutputLimit = 10s;
constexpr double LiveInitialAvSkewSeconds = 1.0;
constexpr double LiveClockJumpSeconds = 10.0;
}

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
    if (!shutting_down_) video_widget_ = widget;
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
    request_submitted_at_ = {};
    live_output_started_at_.reset();
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
}

std::uint64_t PlayerController::requestOpen(const PlaybackSource& source)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return requestOpenLocked(source, false);
}

// 调用者已经持有命令锁。按值接收源，避免继续操作时引用被自身的状态替换失效。
std::uint64_t PlayerController::requestOpenLocked(PlaybackSource source, bool retain_frame)
{
    if (shutting_down_) return 0;
    auto io = std::make_shared<MediaIoControl>();

    const PlaybackEpoch epoch{snapshot_.request_id + 1, 1, 1};
    const auto submitted_at = Clock::now();
    cancelCurrentLocked();
    audio_packets_.abort();
    video_packets_.abort();
    resetSnapshotLocked(epoch.request_id, PlaybackState::Opening);
    request_submitted_at_ = submitted_at;
    snapshot_.connection_attempt_id = epoch.connection_attempt_id;
    snapshot_.generation = epoch.generation;
    snapshot_.source_kind = source.kind();
    snapshot_.protocol = source.protocol();
    if (snapshot_.protocol == PlaybackProtocol::Rtsp && source.liveOptions()) {
        snapshot_.rtsp_transport = source.liveOptions()->rtsp_transport;
    }
    snapshot_.display_name = source.displayName();
    snapshot_.capabilities = initialPlaybackCapabilities(source.kind(), false);
    if (source.kind() == PlaybackSourceKind::NetworkLive) snapshot_.network_stats.emplace();
    resume_state_ = PlaybackState::Playing;
    if (video_widget_) video_widget_->setPlaybackEpoch(epoch, retain_frame);

    // 先发布可取消对象，再唤醒 supervisor。打开尚未开始时也能取消这条命令。
    active_io_ = io;
    current_source_ = source;
    pending_open_ = OpenRequest{std::move(source), epoch, std::move(io)};
    command_changed_.notify_one();
    return epoch.request_id;
}

std::uint64_t PlayerController::stopAsync()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (shutting_down_) return 0;
    const auto request_id = snapshot_.request_id;
    cancelCurrentLocked();
    current_source_.reset();
    audio_packets_.abort();
    video_packets_.abort();
    resetSnapshotLocked(request_id, supervisor_busy_ ? PlaybackState::Stopping : PlaybackState::Idle);
    if (video_widget_) video_widget_->setPlaybackEpoch({}, false);
    return request_id;
}

void PlayerController::requestShutdown()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (shutting_down_) return;
    shutting_down_ = true;
    cancelCurrentLocked();
    current_source_.reset();
    resetSnapshotLocked(snapshot_.request_id, supervisor_busy_ ? PlaybackState::Stopping : PlaybackState::Idle);
    if (video_widget_) video_widget_->setPlaybackEpoch({}, false);
    // FFmpeg 回调轮询取消标志，退出后由 supervisor 中止队列、释放包并回收 worker。
    command_changed_.notify_one();
}

bool PlayerController::isShutdownComplete() const noexcept
{
    return shutdown_complete_.load(std::memory_order_acquire);
}

void PlayerController::shutdown()
{
    requestShutdown();
    if (supervisor_.joinable()) supervisor_.join();
}

std::uint64_t PlayerController::setPausedLocked(bool paused)
{
    if (shutting_down_) return 0;
    if (current_source_ && current_source_->kind() == PlaybackSourceKind::NetworkLive) {
        if (!paused) {
            // 不回调公开 requestOpen，避免重复取得 snapshot_mutex_。
            if (snapshot_.state == PlaybackState::Paused) return requestOpenLocked(*current_source_, true);
            return snapshot_.state == PlaybackState::Opening || snapshot_.state == PlaybackState::Buffering ||
                snapshot_.state == PlaybackState::Playing || snapshot_.state == PlaybackState::Reconnecting
                ? snapshot_.request_id : 0;
        }
        if (snapshot_.state == PlaybackState::Paused) return snapshot_.request_id;
        if (snapshot_.state != PlaybackState::Opening && snapshot_.state != PlaybackState::Buffering &&
            snapshot_.state != PlaybackState::Playing && snapshot_.state != PlaybackState::Reconnecting) return 0;

        // 先冻结可展示进度，再取消旧输入与输出。旧 worker 的发布检查也会立即失效。
        if (live_output_started_at_) {
            snapshot_.live_elapsed_seconds =
                std::chrono::duration<double>(Clock::now() - *live_output_started_at_).count();
            live_output_started_at_.reset();
        }
        cancelCurrentLocked();
        active_io_.reset();
        audio_packets_.abort();
        video_packets_.abort();
        snapshot_.state = PlaybackState::Paused;
        snapshot_.connection_attempt_id = 0;
        snapshot_.generation = 0;
        snapshot_.has_current_video_output = false;
        snapshot_.stats = {};
        snapshot_.last_error.reset();
        paused_ = true;
        if (video_widget_) video_widget_->setPlaybackEpoch({}, true);
        return snapshot_.request_id;
    }
    if (!playing_ || snapshot_.source_kind != PlaybackSourceKind::LocalFile) return 0;
    if (paused_ == paused) return snapshot_.request_id;
    if (paused) {
        resume_state_ = snapshot_.state;
        snapshot_.state = PlaybackState::Paused;
    }
    else {
        snapshot_.state = resume_state_;
    }
    paused_ = paused;
    return snapshot_.request_id;
}

std::uint64_t PlayerController::play()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return setPausedLocked(false);
}

std::uint64_t PlayerController::pause()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return setPausedLocked(true);
}

std::uint64_t PlayerController::togglePlayPause()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return setPausedLocked(!paused_.load());
}

std::uint64_t PlayerController::returnToLive()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (shutting_down_ || !current_source_ || current_source_->kind() != PlaybackSourceKind::NetworkLive) return 0;
    return requestOpenLocked(*current_source_, true);
}

void PlayerController::seek(double seconds)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (shutting_down_ || !playing_ || snapshot_.source_kind == PlaybackSourceKind::NetworkLive ||
        !snapshot_.capabilities.can_seek || !std::isfinite(seconds)) return;
    pending_seek_ = std::clamp(seconds, 0.0, duration_.load());
    seek_requested_ = true;
}

void PlayerController::setVolume(float volume)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (!shutting_down_) volume_ = std::clamp(volume, 0.0f, 1.0f);
}

void PlayerController::setSpeed(float speed)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    // 网络直播固定 1.0x；这里保存的始终是本地文件倍速偏好。
    if (!shutting_down_ && snapshot_.source_kind == PlaybackSourceKind::LocalFile && std::isfinite(speed)) {
        speed_ = std::clamp(speed, 0.25f, 4.0f);
    }
}

void PlayerController::setEffect(VideoEffect effect)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (!shutting_down_) current_effect_ = effect;
}

void PlayerController::toggleVisualizer()
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (!shutting_down_) show_visualizer_ = !show_visualizer_.load();
}

PlaybackSnapshot PlayerController::getPlaybackSnapshot() const
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    PlaybackSnapshot snapshot = snapshot_;
    snapshot.is_playing = playing_.load();
    snapshot.is_paused = paused_.load();
    snapshot.is_session_active = supervisor_busy_;
    snapshot.current_seconds = current_time_.load();
    snapshot.duration_seconds = duration_.load();
    if (live_output_started_at_) {
        snapshot.live_elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - *live_output_started_at_).count();
    }
    if (snapshot.network_stats && snapshot.network_stats->first_video_submit_seconds && video_widget_) {
        // 绘制不回调控制器。读取只匹配当前标签，并按相同的请求时钟计时。
        if (const auto drawn = video_widget_->firstDrawnAt(currentEpochLocked())) {
            snapshot.network_stats->first_video_draw_seconds =
                std::chrono::duration<double>(*drawn - request_submitted_at_).count();
        }
    }
    snapshot.stats.audio_queue_packets = audio_packets_.size();
    snapshot.stats.video_queue_packets = video_packets_.size();
    return snapshot;
}

bool PlayerController::updateLocalLibraryInfo(std::uint64_t expected_request_id, const MediaInfo& info)
{
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (shutting_down_ || expected_request_id == 0 || expected_request_id != snapshot_.request_id ||
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
        if (shutting_down_) {
            active_io_.reset();
            video_widget_ = nullptr;
            snapshot_.state = PlaybackState::Idle;
            break;
        }
        auto request = std::move(*pending_open_);
        pending_open_.reset();
        auto* video = video_widget_;
        supervisor_busy_ = true;
        lock.unlock();

        const auto error = runRequest(request, video);
        decoder_.close();

        lock.lock();
        supervisor_busy_ = false;
        if (shutting_down_) continue;
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
        if (live_output_started_at_) {
            snapshot_.live_elapsed_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - *live_output_started_at_).count();
            live_output_started_at_.reset();
        }
        if (error && request.source.kind() == PlaybackSourceKind::NetworkLive) {
            // 失败后只保留已绘制静帧。先保存真实绘制历史，再撤销待绘制数据和旧输出标签。
            if (video) {
                if (snapshot_.network_stats && !snapshot_.network_stats->first_video_draw_seconds) {
                    if (const auto drawn = video->firstDrawnAt(currentEpochLocked())) {
                        snapshot_.network_stats->first_video_draw_seconds =
                            std::chrono::duration<double>(*drawn - request_submitted_at_).count();
                    }
                }
                video->setPlaybackEpoch({}, true);
            }
            snapshot_.connection_attempt_id = 0;
            snapshot_.generation = 0;
            snapshot_.has_current_video_output = false;
        }
        snapshot_.state = error ? PlaybackState::Failed : PlaybackState::Ended;
        snapshot_.last_error = error;
    }
    // 上一请求已回收 worker 并关闭输入，最后一次访问控件和快照结束后才发布完成。
    shutdown_complete_.store(true, std::memory_order_release);
}

std::optional<PlaybackError> PlayerController::runRequest(const OpenRequest& request, VideoWidget* video)
{
    const auto result = decoder_.open(request.source, request.io);
    if (result.status != MediaOpenStatus::Opened) return result;
    const bool live = request.source.kind() == PlaybackSourceKind::NetworkLive;

    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (request.io->isCancelled() || currentEpochLocked() != request.epoch) return {};
        snapshot_.media_info = decoder_.getMediaInfo();
        duration_ = snapshot_.media_info.duration;
        snapshot_.capabilities = initialPlaybackCapabilities(request.source.kind(), duration_ > 0.0);
        snapshot_.state = live ? PlaybackState::Buffering : PlaybackState::Playing;
        audio_packets_.reset();
        video_packets_.reset();
        playing_ = !live;
        if (live) request.io->beginInitialOutputDeadline(Clock::now() + LiveInitialOutputLimit);
    }

    const auto read_error = demuxAndPlay(request, video);
    if (read_error) return *read_error;
    return {};
}

std::optional<MediaReadResult> PlayerController::demuxAndPlay(const OpenRequest& request, VideoWidget* video)
{
    if (request.io->isCancelled()) return {};
    const bool local_file = request.source.kind() == PlaybackSourceKind::LocalFile;
    const bool live = request.source.kind() == PlaybackSourceKind::NetworkLive;
    const bool use_audio = decoder_.hasAudio();
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
        return running() && !seek_requested_ && !reconfiguring && epoch == epoch_now() &&
            (!live || !request.io->isInitialOutputTimedOut());
    };
    auto publish = [&](PlaybackEpoch epoch, const auto& update) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (is_current(epoch) && epoch == currentEpochLocked()) update();
    };

    // 在匹配标签的快照锁内调用；一轨可以先输出，全部轨就绪后才进入 Playing。
    auto record_live_output = [&](std::optional<double>& first, Clock::time_point at) {
        if (!first) first = std::chrono::duration<double>(at - request_submitted_at_).count();
        if (!live_output_started_at_ || at < *live_output_started_at_) live_output_started_at_ = at;
        const auto& progress = *snapshot_.network_stats;
        if (snapshot_.state == PlaybackState::Buffering &&
            (!use_audio || progress.first_pcm_seconds) && (!use_video || progress.first_video_submit_seconds) &&
            request.io->finishInitialOutput()) {
            snapshot_.state = PlaybackState::Playing;
            playing_ = true;
        }
    };

    struct AudioClockSample {
        PlaybackEpoch epoch;
        std::optional<double> position;
        double queued_seconds = 0.0;
        bool tail_drained = false;
        bool uses_source_pts = false;
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
                if (is_current(epoch)) {
                    if (live && frame.corrupt) {
                        publish(epoch, [&] { ++snapshot_.network_stats->corrupt_frames_skipped; });
                    } else {
                        output(frame, epoch);
                    }
                }
                av_freep(&frame.data);
            }
            if (flushed && is_current(epoch)) drained = epoch.generation;
        }
        av_packet_free(&packet);
    };

    std::thread audio_worker;
    bool audio_ready = false;
    if (use_audio) {
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
            bool clock_has_source_pts = false;
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
                sample.uses_source_pts = clock_has_source_pts;
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
                    clock_has_source_pts = false;
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
                    if (offset > previous_offset && result != AudioWriteResult::Error) {
                        last_write = Clock::now();
                        if (!clock_base) {
                            // 只由实际提交的 PCM 建立声卡主时钟，不用跨轨合成时间戳。
                            clock_has_source_pts = frame.has_valid_pts && std::isfinite(effective_pts);
                            clock_base = live ? (clock_has_source_pts ? effective_pts : 0.0)
                                              : std::max(effective_pts, target);
                            if (live) publish(epoch, [&] {
                                record_live_output(snapshot_.network_stats->first_pcm_seconds, last_write);
                            });
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
    if (live && use_audio && !audio_ready) {
        return MediaReadResult{MediaReadStatus::Error, AVERROR(EIO), MediaReadStage::InitialOutput};
    }
    if (!audio_ready && !use_video) return MediaReadResult{MediaReadStatus::Error, AVERROR(EIO)};

    std::thread video_worker;
    if (use_video) {
        const double nominal_frame_seconds = 1.0 / decoder_.getMediaInfo().video_info.frame_rate;
        video_worker = std::thread([&, nominal_frame_seconds] {
            auto playback_anchor = Clock::now();
            auto window_start = playback_anchor;
            double anchor_pts = current_time_.load();
            std::uint64_t anchor_generation = 0;
            int decoded_frames = 0;
            int submitted_frames = 0;
            std::optional<double> last_live_video_pts;
            std::optional<double> last_live_presentation;
            std::optional<double> video_to_audio_offset;
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
                if (!live && frame.pts + 0.0001 < min_valid_pts.load()) return;
                ++decoded_frames;
                auto audio = read_audio_clock(epoch);
                double presentation_pts = frame.pts;
                if (live) {
                    // 同一连接有音轨时始终以已提交 PCM 为主时钟，视频不能先抢跑。
                    while (audio_ready && !audio.position && is_current(epoch) && !audio_error) {
                        std::this_thread::sleep_for(5ms);
                        audio = read_audio_clock(epoch);
                    }
                    if (!is_current(epoch) || audio_error) return;

                    const bool valid_pts = frame.has_valid_pts && std::isfinite(frame.pts);
                    const bool clock_jump = valid_pts && last_live_video_pts &&
                        std::abs(frame.pts - *last_live_video_pts) > LiveClockJumpSeconds;
                    const bool first_valid_pts = valid_pts && !last_live_video_pts;
                    const auto fallback = audio.position.value_or(0.0);
                    if (valid_pts) {
                        if (audio.position) {
                            // 单轨 PTS 有效不代表两轨共享起点。首次用可信视频 PTS 配对时
                            // 只保留合理的轨间偏移；这里比较的是已经前进的声卡时钟。
                            // 无时间戳的早期画面沿用下方估算，不提前锁定两轨映射。
                            if (!video_to_audio_offset || clock_jump) {
                                const double delta = frame.pts - *audio.position;
                                video_to_audio_offset = !clock_jump && audio.uses_source_pts &&
                                    std::abs(delta) <= LiveInitialAvSkewSeconds ? 0.0 : -delta;
                            }
                            presentation_pts += *video_to_audio_offset;
                        }
                        last_live_video_pts = frame.pts;
                    } else {
                        presentation_pts = last_live_presentation
                            ? *last_live_presentation + nominal_frame_seconds : fallback;
                    }
                    if (!audio.position && (!last_live_presentation || first_valid_pts || clock_jump)) {
                        // 纯视频首帧贴合本地单调时钟，后续只等待相对差，避免按绝对 PTS 等待。
                        playback_anchor = Clock::now();
                        anchor_pts = presentation_pts;
                    }
                    last_live_presentation = presentation_pts;
                }
                const bool sync_to_audio = audio.position.has_value();
                double diff = 0.0;
                if (sync_to_audio) {
                    diff = presentation_pts - *audio.position;
                    while (is_current(epoch) && !paused_ && !speed_change_pending && !audio_error && diff > 0.04) {
                        if (audio.tail_drained) break;
                        std::this_thread::sleep_for(5ms);
                        audio = read_audio_clock(epoch);
                        if (!audio.position) break;
                        diff = presentation_pts - *audio.position;
                    }
                }
                else if (!audio_ready || local_file) {
                    // 本地源刚刷新时，声卡尚无新时钟，先用视频节奏避免一口气提交预读帧。
                    const double delay = (presentation_pts - anchor_pts) / active_speed.load();
                    if (delay > 0.0) {
                        const auto at = playback_anchor + std::chrono::duration_cast<Clock::duration>(
                            std::chrono::duration<double>(delay));
                        while (is_current(epoch) && !paused_ && !speed_change_pending && Clock::now() < at) {
                            std::this_thread::sleep_for(1ms);
                        }
                    }
                }
                if (!is_current(epoch) || paused_ || speed_change_pending || audio_error) return;

                const bool dropped = sync_to_audio && diff < -0.12;
                bool accepted = false;
                auto submitted_at = Clock::time_point{};
                if (!dropped && frame.data && video) {
                    const uint8_t* planes[3] = {frame.planes[0], frame.planes[1], frame.planes[2]};
                    submitted_at = Clock::now();
                    accepted = video->setVideoFrame(epoch, frame.video_format, planes, frame.linesizes,
                        frame.video_full_range, frame.video_bt709, frame.width, frame.height);
                    if (accepted) ++submitted_frames;
                }

                const auto now = Clock::now();
                const double elapsed = std::chrono::duration<double>(now - window_start).count();
                publish(epoch, [&] {
                    if (!paused_ && (audio_clock.epoch != epoch || !audio_clock.position)) {
                        current_time_ = presentation_pts;
                    }
                    if (accepted) {
                        snapshot_.has_current_video_output = true;
                        if (live) record_live_output(snapshot_.network_stats->first_video_submit_seconds, submitted_at);
                    }
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
    bool awaiting_video_key = live && use_video;
    const auto initial_output_timeout = MediaReadResult{
        MediaReadStatus::Timeout, AVERROR(ETIMEDOUT), MediaReadStage::InitialOutput};

    while (packet && running()) {
        if (live && request.io->isInitialOutputTimedOut()) {
            error = initial_output_timeout;
            break;
        }
        if (worker_allocation_failed) {
            error = MediaReadResult{MediaReadStatus::Error, AVERROR(ENOMEM)};
            break;
        }
        if (const int code = audio_error.load()) {
            error = MediaReadResult{MediaReadStatus::Error, code,
                live && !playing_ ? MediaReadStage::InitialOutput : MediaReadStage::ReadPacket};
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
        if (live && request.io->isInitialOutputTimedOut()) {
            error = initial_output_timeout;
            if (result.ffmpegError < 0) error->ffmpegError = result.ffmpegError;
            break;
        }
        if (result.status == MediaReadStatus::Again) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (!running()) break;
        if (result.status == MediaReadStatus::Packet) {
            if (live && !snapshot_.network_stats->first_packet_seconds) {
                snapshot_.network_stats->first_packet_seconds =
                    std::chrono::duration<double>(Clock::now() - request_submitted_at_).count();
            }
            // 读包前捕获标签，入队时保存。暂停可保留队列，seek 则丢弃这次旧读取。
            if (!seek_requested_) {
                if (audio_ready && decoder_.isAudioPacket(packet)) audio_packets_.push(packet, epoch);
                else if (use_video && decoder_.isVideoPacket(packet)) {
                    // 解码器须从关键包提示开始；坏包标记的首个关键包不打开门。
                    const bool clean_key = (packet->flags & AV_PKT_FLAG_KEY) && !(packet->flags & AV_PKT_FLAG_CORRUPT);
                    if ((!awaiting_video_key || clean_key) && video_packets_.push(packet, epoch)) {
                        awaiting_video_key = false;
                    }
                }
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
