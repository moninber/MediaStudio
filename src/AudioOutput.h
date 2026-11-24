/**
 * @file AudioOutput.h
 * @brief 音频输出 - 基于Windows WASAPI的音频播放
 * 
 * WASAPI (Windows Audio Session API) 是Windows Vista及之后版本的核心音频API：
 * - 低延迟音频输出
 * - 共享模式/独占模式
 * - 支持多种音频格式
 * 
 * WASAPI播放流程：
 * 1. 获取默认音频端点设备（IMMDevice）
 * 2. 激活音频客户端（IAudioClient）
 * 3. 设置音频格式（WAVEFORMATEX）
 * 4. 初始化音频客户端（分配缓冲区）
 * 5. 获取渲染客户端（IAudioRenderClient）
 * 6. 循环：请求缓冲区 → 写入PCM数据 → 释放缓冲区
 * 
 * COM (Component Object Model) 说明：
 * - WASAPI基于COM，使用前需调用CoInitializeEx()
 * - COM对象使用Release()释放（类似智能指针的引用计数）
 * - COM接口使用HRESULT返回状态码
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // 禁用min/max宏，避免与std::min/std::max冲突
#endif

#include <windows.h>
#include <mmdeviceapi.h>    // IMMDeviceEnumerator, IMMDevice
#include <audioclient.h>    // IAudioClient, IAudioRenderClient
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include "AudioBufferSettings.h"

// 区分 PCM 已提交、部分提交、声卡暂满、命令中断和设备失败。
enum class AudioWriteResult { Written, Pending, WouldBlock, Interrupted, Error };

// 使用同一次声卡采样计算的已播放时长和待播放时长。
struct AudioProgress {
    double played_seconds = 0.0;
    double queued_seconds = 0.0;
    bool valid = false;
};

// 音频工作线程独占对象，从构造、COM 初始化到析构均在同一线程完成。
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    /**
     * @brief 初始化音频输出设备
     * @param sample_rate 采样率（Hz）
     * @param channels 声道数
     * @return true=成功, false=失败
     */
    bool init(int sample_rate, int channels, AudioBufferRequest buffer = {},
              const std::function<bool()>& can_continue = {});
    // 只由设备线程读取，再按值发布给控制器；cleanup 后仍可保留最近初始化的诊断历史。
    AudioBufferReport bufferReport() const;

    /** @brief 释放WASAPI资源 */
    void cleanup();

    // 每次最多提交约 10 ms PCM，只在提交成功后推进帧偏移。调用者在两次写入之间处理命令。
    // can_write 快速查询当前取消、暂停和代次状态，不等待且不抛异常。
    AudioWriteResult write(const std::vector<std::uint8_t>& data, std::size_t& offset_frames,
        const std::function<bool()>& can_write);

    // 用一次 padding 查询生成快照，valid 为 false 代表本次无法采样。
    AudioProgress sample();

    // 暂停时保留设备里已提交但尚未播放的 PCM，继续后从剩余位置播放。
    bool pause();
    bool resume();

    // 清除设备中的待播放 PCM 和累计帧数，保留当前暂停状态。
    bool reset();

private:
    friend struct AudioOutputTestAccess;
    // 默认创建真实设备客户端；测试可用 COM 代理拒绝首次请求，核对失败后的释放/重建边界。
    using ClientFactory = std::function<HRESULT(IMMDevice*, IAudioClient**)>;
    ClientFactory client_factory_;
    HRESULT activateClient();
    void releaseClient();
    // 调试构建检查所有设备调用来自构造线程。
    void assertOwnerThread() const noexcept;

    // WASAPI COM接口指针
    IMMDeviceEnumerator* device_enumerator_;  // 设备枚举器（用于查找音频设备）
    IMMDevice* device_;                       // 音频端点设备（扬声器/耳机）
    IAudioClient* audio_client_;              // 音频客户端（管理音频流）
    IAudioRenderClient* render_client_;       // 渲染客户端（写入PCM数据）

    // 音频格式参数
    WAVEFORMATEX* wave_format_;               // 音频格式描述结构

    // 状态只由设备所属的音频工作线程访问。
    bool playing_;
    bool paused_;
    std::uint64_t submitted_frames_;
    bool com_initialized_;                     // 是否由本对象初始化了COM
    UINT32 buffer_frame_count_;                // 实际 WASAPI 缓冲区帧数
    AudioBufferReport buffer_report_;
#ifndef NDEBUG
    const DWORD owner_thread_id_ = GetCurrentThreadId();
#endif
};
