/**
 * @file VideoWidget.cpp
 * @brief 视频显示控件实现
 */

#include "VideoWidget.h"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <utility>

VideoWidget::VideoWidget(QWidget* parent)
    : QOpenGLWidget(parent)
    , gl_initialized_(false)
    , show_spectrum_(false)
{
    // 设置控件属性
    setMinimumSize(640, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::NoFocus);  // 禁止 OpenGL 窗口拦截焦点，让父窗口 MainWindow 统一处理按键

    // 启动刷新定时器（约60fps）
    // QTimer在Qt事件循环中触发，确保paintGL()在正确的线程调用
    QObject::connect(&refresh_timer_, &QTimer::timeout, this, [this]() {
        bool repaint;
        {
            std::lock_guard lock(output_mutex_);
            repaint = needs_update_;
        }
        if (repaint) {
            update();  // 请求重绘，Qt会在合适时机调用paintGL()
        }
    });
    refresh_timer_.start(16);  // ~60fps
}

VideoWidget::~VideoWidget()
{
    // 确保OpenGL资源在上下文有效时释放
    makeCurrent();
    renderer_.reset();
    visualizer_.reset();
    doneCurrent();
}

void VideoWidget::initializeGL()
{
    // 初始化Qt的OpenGL函数指针（QOpenGLFunctions方法调用前必须先调用）
    initializeOpenGLFunctions();

    // 再初始化GLEW（VideoRenderer/AudioVisualizer使用GLEW的函数指针）
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        std::cerr << "[VideoWidget] GLEW初始化失败: " << glewGetErrorString(err) << std::endl;
        return;
    }
    // glewInit可能设置GL_INVALID_ENUM，清除错误
    glGetError();

    glDisable(GL_DEPTH_TEST);

    // 输出OpenGL信息
    std::cout << "[VideoWidget] OpenGL版本: " << glGetString(GL_VERSION) << std::endl;

    // 创建并初始化渲染器
    renderer_ = std::make_unique<VideoRenderer>();
    visualizer_ = std::make_unique<AudioVisualizer>();

    // 初始大小设为默认值，加载文件后会重新初始化
    renderer_->init(1920, 1080);
    visualizer_->init(64);
    displayed_spectrum_epoch_.reset();

    gl_initialized_ = true;
    std::cout << "[VideoWidget] OpenGL初始化完成" << std::endl;
}

void VideoWidget::paintGL()
{
    if (!gl_initialized_) return;

    const bool spectrum_mode = show_spectrum_;
    PlaybackEpoch epoch;
    std::uint64_t reset;
    std::vector<float> spectrum;
    bool draw_pending_video = false;
    {
        std::lock_guard lock(output_mutex_);
        epoch = active_epoch_;
        reset = display_reset_;
        needs_update_ = false;
        if (spectrum_mode && spectrum_epoch_ == epoch) {
            spectrum = pending_spectrum_;
            pending_spectrum_.clear();
        } else if (!spectrum_mode && has_pending_video_ && pending_video_.epoch == epoch) {
            std::swap(drawing_video_, pending_video_);
            has_pending_video_ = false;
            draw_pending_video = true;
        }
    }

    const qreal dpr = devicePixelRatioF();
    const int viewport_width = static_cast<int>(width() * dpr);
    const int viewport_height = static_cast<int>(height() * dpr);

    if (spectrum_mode) {
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    } else {
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    // 只有已经绘制过的帧可以跨代静态显示，待绘制帧和频谱永远按完整标签检查。
    if (spectrum_mode) {
        const bool continuing_spectrum = displayed_spectrum_epoch_ == epoch && displayed_spectrum_reset_ == reset;
        if (!visualizer_ || (spectrum.empty() && !continuing_spectrum)) return;
        if (!continuing_spectrum) visualizer_->resetSpectrum();
        if (!spectrum.empty()) visualizer_->setSpectrumData(spectrum);
        visualizer_->render(viewport_width, viewport_height);
    } else {
        const auto& frame = draw_pending_video ? drawing_video_ : displayed_video_;
        if (!renderer_ || frame.width == 0 || frame.display_reset != reset) return;
        renderer_->setViewport(0, 0, viewport_width, viewport_height);
        if (frame.format == VideoFrameFormat::YUV420P) {
            renderer_->renderYUV420P(frame.planes[0].data(), frame.planes[1].data(), frame.planes[2].data(),
                                    frame.full_range, frame.bt709, frame.width, frame.height);
        } else {
            renderer_->renderRGBA(frame.planes[0].data(), frame.width, frame.height);
        }
    }

    bool still_eligible;
    {
        // 绘制期间若被切源或 seek 取代，不将其升级为可保留的帧，并清除这次绘制。
        std::lock_guard lock(output_mutex_);
        still_eligible = spectrum_mode || draw_pending_video
            ? active_epoch_ == epoch && display_reset_ == reset : display_reset_ == reset;
        if (still_eligible) {
            if (spectrum_mode) {
                displayed_spectrum_epoch_ = epoch;
                displayed_spectrum_reset_ = reset;
            } else if (draw_pending_video) {
                std::swap(displayed_video_, drawing_video_);
            }
        }
    }
    if (!still_eligible) glClear(GL_COLOR_BUFFER_BIT);
}

void VideoWidget::resizeGL(int w, int h)
{
    if (renderer_) {
        renderer_->resize(w, h);
    }
}

// 更新代标签与清除策略，不在工作线程调用 Qt 或 GL。
void VideoWidget::setPlaybackEpoch(PlaybackEpoch epoch, bool retain_frame)
{
    std::lock_guard lock(output_mutex_);
    if (active_epoch_ == epoch && retain_frame) return;
    active_epoch_ = epoch;
    if (!retain_frame) ++display_reset_;
    has_pending_video_ = false;
    pending_spectrum_.clear();
    needs_update_ = true;
}

// 接受时按完整标签检查，像素仍归调用方所有，控件只保存自己的副本。
bool VideoWidget::setVideoFrame(PlaybackEpoch epoch, VideoFrameFormat format, const uint8_t* const planes[3],
    const int linesizes[3], bool full_range, bool bt709, int width, int height)
{
    if (!planes || !linesizes || width <= 0 || height <= 0) return false;
    const bool yuv = format == VideoFrameFormat::YUV420P;
    const int count = yuv ? 3 : 1;
    const int chroma_width = width / 2 + width % 2;
    const int chroma_height = height / 2 + height % 2;
    const std::ptrdiff_t plane_widths[3] = {
        static_cast<std::ptrdiff_t>(width) * (yuv ? 1 : 4), chroma_width, chroma_width
    };
    const int plane_heights[3] = { height, chroma_height, chroma_height };
    for (int plane = 0; plane < count; ++plane) {
        const auto stride = static_cast<std::ptrdiff_t>(linesizes[plane]);
        if (!planes[plane] || (stride > -plane_widths[plane] && stride < plane_widths[plane])) return false;
    }

    std::lock_guard lock(output_mutex_);
    if (epoch != active_epoch_) return false;
    for (int plane = 0; plane < count; ++plane) {
        const auto row_size = plane_widths[plane];
        auto& target = pending_video_.planes[plane];
        target.resize(static_cast<std::size_t>(row_size) * plane_heights[plane]);
        for (int row = 0; row < plane_heights[plane]; ++row) {
            const auto offset = static_cast<std::ptrdiff_t>(row);
            std::memcpy(target.data() + offset * row_size, planes[plane] + offset * linesizes[plane], row_size);
        }
    }
    pending_video_.epoch = epoch;
    pending_video_.display_reset = display_reset_;
    pending_video_.format = format;
    pending_video_.full_range = full_range;
    pending_video_.bt709 = bt709;
    pending_video_.width = width;
    pending_video_.height = height;
    has_pending_video_ = true;
    needs_update_ = true;
    return true;
}

// 工作线程只提交频谱数据，实际更新可视化器由 paintGL 在 Qt 线程完成。
bool VideoWidget::setSpectrumData(PlaybackEpoch epoch, const std::vector<float>& spectrum)
{
    std::lock_guard lock(output_mutex_);
    if (epoch != active_epoch_) return false;
    spectrum_epoch_ = epoch;
    pending_spectrum_ = spectrum;
    needs_update_ = true;
    return true;
}

void VideoWidget::setShowSpectrum(bool show)
{
    show_spectrum_ = show;
    update();
}

void VideoWidget::setEffect(VideoEffect effect)
{
    if (renderer_) {
        renderer_->setEffect(effect);
        update();
    }
}
