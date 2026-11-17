/**
 * @file VideoWidget.h
 * @brief 视频显示控件 - 基于QOpenGLWidget的视频渲染与频谱可视化
 * 
 * QOpenGLWidget是Qt提供的OpenGL渲染控件：
 * - 自动管理OpenGL上下文的生命周期
 * - 与Qt事件循环无缝集成
 * - 支持在Qt布局系统中自由放置
 * 
 * 渲染流程：
 * 1. initializeGL() → 创建着色器、纹理、几何体（仅调用一次）
 * 2. resizeGL()     → 调整视口大小
 * 3. paintGL()      → 每帧渲染（由update()触发）
 * 
 * 帧数据传递：
 * - PlayerController在解码线程中存储RGB帧数据到共享缓冲区
 * - VideoWidget通过QTimer定期调用update()触发重绘
 * - paintGL()中从缓冲区读取最新帧并渲染
 * 
 * 这种设计确保：
 * - OpenGL调用只在Qt线程（拥有OpenGL上下文的线程）中执行
 * - 解码线程不接触任何OpenGL对象
 * - 线程安全通过mutex保护共享缓冲区
 */

#pragma once

// GLEW必须在所有OpenGL头文件之前包含（包括Qt的OpenGL头）
// 否则会报错: "gl.h included before glew.h"
#include <GL/glew.h>

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QTimer>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "PlaybackEpoch.h"
#include "VideoRenderer.h"
#include "AudioVisualizer.h"

class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget();

    // 同步更新输出归属，旧帧和旧频谱从这一刻起不能作为本代输出提交。
    void setPlaybackEpoch(PlaybackEpoch epoch, bool retain_frame);

    // 仅返回当前代首帧的 GL 绘制时刻；线程安全，不调用 Qt 事件或 OpenGL。
    std::optional<std::chrono::steady_clock::time_point> firstDrawnAt(PlaybackEpoch epoch) const;

    /**
     * @brief 设置视频帧数据（由PlayerController调用，线程安全）
     * @return 帧属于当前播放代且已放入待绘制缓冲区
     */
    bool setVideoFrame(PlaybackEpoch epoch, VideoFrameFormat format, const uint8_t* const planes[3],
        const int linesizes[3], bool full_range, bool bt709, int width, int height);

    /**
     * @brief 设置频谱数据（由PlayerController调用，线程安全）
     * @param spectrum 频谱数据（归一化振幅0~1）
     */
    bool setSpectrumData(PlaybackEpoch epoch, const std::vector<float>& spectrum);

    /** @brief 切换视频/频谱显示模式 */
    void setShowSpectrum(bool show);

    /** @brief 设置视频特效 */
    void setEffect(VideoEffect effect);

protected:
    // QOpenGLWidget虚函数
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    struct VideoFrameBuffer {
        PlaybackEpoch epoch;
        std::uint64_t display_reset = 0;
        std::vector<uint8_t> planes[3];
        VideoFrameFormat format = VideoFrameFormat::RGBA;
        bool full_range = true;
        bool bt709 = false;
        int width = 0;
        int height = 0;
    };

    // OpenGL渲染器
    std::unique_ptr<VideoRenderer> renderer_;
    std::unique_ptr<AudioVisualizer> visualizer_;
    bool gl_initialized_;

    // 生产线程只访问锁保护的待绘制数据，OpenGL 绘制不持有此锁。
    mutable std::mutex output_mutex_;
    PlaybackEpoch active_epoch_{};
    std::optional<std::chrono::steady_clock::time_point> first_drawn_at_;
    // 每次要求清除画面都推进此序号，使旧静态帧在后续保留操作中也无法恢复。
    std::uint64_t display_reset_ = 0;
    VideoFrameBuffer pending_video_;
    bool has_pending_video_ = false;
    PlaybackEpoch spectrum_epoch_{};
    std::vector<float> pending_spectrum_;
    bool needs_update_ = false;

    // 这些缓冲区和显示模式只由 Qt 线程使用。
    VideoFrameBuffer drawing_video_;
    VideoFrameBuffer displayed_video_;
    std::optional<PlaybackEpoch> displayed_spectrum_epoch_;
    std::uint64_t displayed_spectrum_reset_ = 0;
    bool show_spectrum_;

    // 刷新定时器
    QTimer refresh_timer_;
};
