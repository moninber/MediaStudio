/**
 * @file AudioOutput.cpp
 * @brief 音频输出实现 - WASAPI音频播放的具体实现
 */

#include "AudioOutput.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <limits>
#include <mmreg.h>      // WAVE_FORMAT_PCM等常量定义
#include <algorithm>    // std::min, std::max

// 初始化拥有资源的状态，设备在音频线程调用 init 后才打开。
AudioOutput::AudioOutput()
    : device_enumerator_(nullptr)
    , device_(nullptr)
    , audio_client_(nullptr)
    , render_client_(nullptr)
    , wave_format_(nullptr)
    , playing_(false)
    , paused_(false)
    , submitted_frames_(0)
    , com_initialized_(false)
    , buffer_frame_count_(0)
{
}

// 析构时在设备所属线程关闭 WASAPI 和 COM，也处理部分初始化失败。
AudioOutput::~AudioOutput()
{
    cleanup();
}

void AudioOutput::assertOwnerThread() const noexcept
{
    assert(owner_thread_id_ == GetCurrentThreadId());
}

// 保留共享模式 16 位 PCM，由预设指定缓冲请求；初始化、回退和释放均留在音频线程。
bool AudioOutput::init(int sample_rate, int channels, AudioBufferRequest buffer,
                       const std::function<bool()>& can_continue)
{
    cleanup();
    buffer_report_ = {};
    buffer_report_.requested = buffer.duration;
    const auto cancelled = [&] {
        if (!can_continue || can_continue()) return false;
        buffer_report_.cancelled = true;
        return true;
    };
    const auto fail = [&](HRESULT error, const char* reason) {
        if (!buffer_report_.first_error) buffer_report_.first_error = static_cast<std::int32_t>(error);
        buffer_report_.last_error = static_cast<std::int32_t>(error);
        std::cerr << "[AudioOutput] " << reason << " (hr=0x" << std::hex << static_cast<std::uint32_t>(error) << std::dec << ")" << std::endl;
        cleanup();
        return false;
    };
    const auto valid_duration = [](std::chrono::milliseconds duration) {
        return duration.count() > 0 && duration.count() <= std::numeric_limits<REFERENCE_TIME>::max() / 10000;
    };
    if (!valid_duration(buffer.duration) || (buffer.fallback &&
        (!valid_duration(*buffer.fallback) || *buffer.fallback <= buffer.duration))) {
        return fail(E_INVALIDARG, "音频缓冲请求无效");
    }
    if (cancelled()) return false;

    constexpr unsigned int bytes_per_sample = 2;
    if (sample_rate <= 0 || channels <= 0 ||
        static_cast<unsigned int>(channels) > std::numeric_limits<WORD>::max() / bytes_per_sample ||
        static_cast<std::uint64_t>(sample_rate) * channels * bytes_per_sample > std::numeric_limits<DWORD>::max()) {
        return fail(E_INVALIDARG, "PCM 格式参数无效");
    }

    // 后台线程需要自己初始化COM（WASAPI是COM对象）
    // COINIT_MULTITHREADED适合后台工作线程
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_hr)) {
        return fail(com_hr, "初始化 COM 失败");
    }
    com_initialized_ = true;

    // --------------------------------------------------------
    // 步骤1：创建设备枚举器
    // MMDeviceEnumerator是WASAPI的入口点
    // 通过它可以枚举系统中的所有音频设备
    // --------------------------------------------------------
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),  // COM类ID
        nullptr,                        // 无外部未知
        CLSCTX_ALL,                     // 所有执行上下文
        __uuidof(IMMDeviceEnumerator),  // 接口ID
        (void**)&device_enumerator_     // 输出接口指针
    );
    if (FAILED(hr)) {
        return fail(hr, "创建设备枚举器失败");
    }

    // --------------------------------------------------------
    // 步骤2：获取默认音频渲染端点
    // eRender = 音频输出设备（扬声器/耳机）
    // eConsole = 控制台应用程序角色
    // --------------------------------------------------------
    hr = device_enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
        return fail(hr, "获取默认音频端点失败");
    }

    // --------------------------------------------------------
    // 步骤3：激活音频客户端
    // IAudioClient是WASAPI的核心接口
    // 它管理音频流的格式、缓冲区和状态
    // --------------------------------------------------------
    hr = activateClient();
    if (FAILED(hr)) {
        return fail(hr, "激活音频客户端失败");
    }

    // --------------------------------------------------------
    // 步骤4：设置音频格式
    // WAVEFORMATEX描述PCM音频格式：
    // - wFormatTag = WAVE_FORMAT_PCM (未压缩PCM)
    // - nChannels = 声道数
    // - nSamplesPerSec = 采样率
    // - wBitsPerSample = 位深度（16位）
    // - nBlockAlign = 每样本字节数 = (wBitsPerSample/8) * nChannels
    // - nAvgBytesPerSec = 每秒字节数 = nSamplesPerSec * nBlockAlign
    // --------------------------------------------------------
    WAVEFORMATEX desired_format = {};
    desired_format.wFormatTag = WAVE_FORMAT_PCM;
    desired_format.nChannels = static_cast<WORD>(channels);
    desired_format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    desired_format.wBitsPerSample = 16;
    desired_format.nBlockAlign = (desired_format.wBitsPerSample / 8) * desired_format.nChannels;
    desired_format.nAvgBytesPerSec = desired_format.nSamplesPerSec * desired_format.nBlockAlign;
    desired_format.cbSize = 0;

    // 检查格式是否被设备支持
    // WASAPI在共享模式下可能需要使用最接近的匹配格式
    WAVEFORMATEX* closest_match = nullptr;
    hr = audio_client_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &desired_format, &closest_match);

    if (hr == S_FALSE && closest_match) {
        wave_format_ = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!wave_format_) {
            CoTaskMemFree(closest_match);
            return fail(E_OUTOFMEMORY, "无法分配音频格式");
        }
        *wave_format_ = desired_format;
        CoTaskMemFree(closest_match);
    }
    else if (hr == S_OK) {
        // 请求的格式完全支持
        wave_format_ = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!wave_format_) {
            return fail(E_OUTOFMEMORY, "无法分配音频格式");
        }
        *wave_format_ = desired_format;
    }
    else {
        if (closest_match) CoTaskMemFree(closest_match);
        return fail(FAILED(hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT, "音频格式不支持");
    }

    // --------------------------------------------------------
    // 步骤5：初始化音频客户端
    // 分配音频缓冲区，指定缓冲持续时间
    // REFERENCE_TIME单位：100纳秒（1秒 = 10,000,000 × 100ns）
    // --------------------------------------------------------
    const auto initialize = [&](std::chrono::milliseconds duration) {
        buffer_report_.attempted = duration;
        ++buffer_report_.attempts;
        const REFERENCE_TIME ticks = duration.count() * 10000; // 明确从 ms 转成 100 ns。
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        return audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, ticks, 0, wave_format_, nullptr);
    };
    if (cancelled()) { cleanup(); return false; }
    hr = initialize(buffer.duration);
    if (FAILED(hr) && buffer.fallback) {
        buffer_report_.first_error = static_cast<std::int32_t>(hr);
        // 失败的 IAudioClient 不能可靠地再次 Initialize；先释放旧客户端，再激活新对象。
        releaseClient();
        if (cancelled()) { cleanup(); return false; }
        hr = activateClient();
        if (FAILED(hr)) return fail(hr, "回退时无法创建新音频客户端");
        if (cancelled()) { cleanup(); return false; }
        std::cerr << "[AudioOutput] " << buffer.duration.count() << " ms 请求失败，尝试回退 "
                  << buffer.fallback->count() << " ms (hr=0x" << std::hex
                  << static_cast<std::uint32_t>(*buffer_report_.first_error) << std::dec << ")" << std::endl;
        hr = initialize(*buffer.fallback);
    }
    if (FAILED(hr)) return fail(hr, "初始化音频缓冲失败");
    if (cancelled()) { cleanup(); return false; }
    hr = audio_client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_client_);
    if (FAILED(hr)) return fail(hr, "获取渲染客户端失败");
    hr = audio_client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr) || buffer_frame_count_ == 0) return fail(FAILED(hr) ? hr : E_FAIL, "无法取得音频缓冲区大小");
    buffer_report_.initialized = true;
    buffer_report_.device_frames = buffer_frame_count_;
    buffer_report_.sample_rate = wave_format_->nSamplesPerSec;
    buffer_report_.capacity_seconds = static_cast<double>(buffer_frame_count_) / wave_format_->nSamplesPerSec;
    std::cout << "[AudioOutput] 初始化成功: " << sample_rate << "Hz, " << channels << "ch, 16bit, requested_ms="
              << buffer_report_.requested.count() << " selected_ms=" << buffer_report_.attempted.count()
              << " device_frames=" << buffer_frame_count_ << " capacity_ms=" << *buffer_report_.capacity_seconds * 1000.0
              << " fallback=" << buffer_report_.usedFallback() << std::endl;

    return true;
}

// 激活、释放及诊断读取都在构造线程执行，不把 COM 接口暴露给 GUI。
HRESULT AudioOutput::activateClient()
{
    assertOwnerThread();
    return client_factory_ ? client_factory_(device_, &audio_client_)
        : device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client_));
}

void AudioOutput::releaseClient()
{
    assertOwnerThread();
    if (audio_client_ && playing_) audio_client_->Stop();
    if (render_client_) { render_client_->Release(); render_client_ = nullptr; }
    if (audio_client_) {
        audio_client_->Release(); audio_client_ = nullptr;
    }
    playing_ = paused_ = false;
    submitted_frames_ = buffer_frame_count_ = 0;
}

AudioBufferReport AudioOutput::bufferReport() const
{
    assertOwnerThread();
    return buffer_report_;
}

// 停止设备后依次释放 COM 接口和格式，再平衡本线程的 CoInitializeEx。
void AudioOutput::cleanup()
{
    assertOwnerThread();
    releaseClient();
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (device_enumerator_) {
        device_enumerator_->Release();
        device_enumerator_ = nullptr;
    }

    // 释放WAVEFORMATEX（使用COM分配器分配的内存）
    if (wave_format_) {
        CoTaskMemFree(wave_format_);
        wave_format_ = nullptr;
    }

    // 平衡COM初始化
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

// 尝试提交一个有界片段，把是否继续等待以及如何处理中断交给音频工作线程。
AudioWriteResult AudioOutput::write(const std::vector<std::uint8_t>& data, std::size_t& offset_frames,
    const std::function<bool()>& can_write)
{
    assertOwnerThread();
    if (!audio_client_ || !render_client_ || !wave_format_ || buffer_frame_count_ == 0) {
        return AudioWriteResult::Error;
    }
    if (paused_ || (can_write && !can_write())) return AudioWriteResult::Interrupted;

    const auto block_align = wave_format_->nBlockAlign;
    const auto total_frames = data.size() / block_align;
    if (data.size() % block_align != 0 || offset_frames > total_frames) return AudioWriteResult::Error;
    if (offset_frames == total_frames) return AudioWriteResult::Written;

    UINT32 padding = 0;
    if (FAILED(audio_client_->GetCurrentPadding(&padding))) return AudioWriteResult::Error;
    if (padding >= buffer_frame_count_) return AudioWriteResult::WouldBlock;

    const auto max_chunk_frames = std::max<DWORD>(1, wave_format_->nSamplesPerSec / 100);
    const auto frames = static_cast<UINT32>(std::min<std::size_t>({
        buffer_frame_count_ - padding, max_chunk_frames, total_frames - offset_frames}));
    BYTE* destination = nullptr;
    if (FAILED(render_client_->GetBuffer(frames, &destination))) return AudioWriteResult::Error;
    if (!destination) {
        render_client_->ReleaseBuffer(0, 0);
        return AudioWriteResult::Error;
    }
    std::memcpy(destination, data.data() + offset_frames * block_align,
                static_cast<std::size_t>(frames) * block_align);
    // 共享模式允许用零帧归还尚未提交的缓冲。代次或暂停在拷贝期间变化时，帧偏移保持不变。
    if (can_write && !can_write()) {
        return SUCCEEDED(render_client_->ReleaseBuffer(0, 0)) ? AudioWriteResult::Interrupted : AudioWriteResult::Error;
    }
    if (FAILED(render_client_->ReleaseBuffer(frames, 0))) return AudioWriteResult::Error;

    offset_frames += frames;
    submitted_frames_ += frames;
    if (!playing_) {
        if (FAILED(audio_client_->Start())) return AudioWriteResult::Error;
        playing_ = true;
    }
    return offset_frames == total_frames ? AudioWriteResult::Written : AudioWriteResult::Pending;
}

// 只读取一次 padding，确保时钟与缓冲统计描述同一个设备时刻。
AudioProgress AudioOutput::sample()
{
    assertOwnerThread();
    if (!audio_client_ || !wave_format_ || buffer_frame_count_ == 0) return {};
    UINT32 padding = 0;
    if (FAILED(audio_client_->GetCurrentPadding(&padding))) return {};

    const auto played = submitted_frames_ > padding ? submitted_frames_ - padding : 0;
    const double sample_rate = wave_format_->nSamplesPerSec;
    return {played / sample_rate, padding / sample_rate, true};
}

// 清空声卡待播帧后清零时钟，等待线程下一次提交有效 PCM 时重新启动设备。
bool AudioOutput::reset()
{
    assertOwnerThread();
    if (!audio_client_) return false;
    if (playing_ && FAILED(audio_client_->Stop())) return false;
    playing_ = false;
    if (FAILED(audio_client_->Reset())) return false;
    submitted_frames_ = 0;
    return true;
}

// Stop 暂停音频引擎，保留已排队数据和累计提交帧，继续时复用原时钟。
bool AudioOutput::pause()
{
    assertOwnerThread();
    if (!audio_client_) return false;
    if (paused_) return true;
    if (playing_ && FAILED(audio_client_->Stop())) return false;
    playing_ = false;
    paused_ = true;
    return true;
}

// 恢复已经提交的 PCM。空设备在下一次有效写入时再启动。
bool AudioOutput::resume()
{
    assertOwnerThread();
    if (!audio_client_) return false;
    if (!paused_) return true;
    if (submitted_frames_ != 0) {
        if (FAILED(audio_client_->Start())) return false;
        playing_ = true;
    }
    paused_ = false;
    return true;
}
