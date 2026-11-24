#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// 请求与设备实际容量分开；文件和稳定模式保留 350 ms，不自动改变 PCM 格式。
struct AudioBufferRequest {
    std::chrono::milliseconds duration{350};
    std::optional<std::chrono::milliseconds> fallback;
};

// 音频线程按值发布最近一次初始化结果。实际容量来自 GetBufferSize，不是当前 padding。
struct AudioBufferReport {
    std::chrono::milliseconds requested{0};
    std::chrono::milliseconds attempted{0};
    unsigned int attempts = 0;
    bool initialized = false;
    bool cancelled = false;
    std::optional<std::int32_t> first_error;
    std::optional<std::int32_t> last_error;
    std::uint32_t device_frames = 0;
    std::uint32_t sample_rate = 0;
    std::optional<double> capacity_seconds;

    bool usedFallback() const noexcept { return initialized && attempts > 1; }
};
