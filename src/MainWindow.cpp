/**
 * @file MainWindow.cpp
 * @brief 主窗口实现 - 完整的Qt桌面媒体播放器界面
 */

#include "MainWindow.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPointer>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QFont>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QCheckBox>
#include <QShortcut>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

namespace {

constexpr std::array<float, 8> kPlaybackSpeeds{0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 4.0f};
constexpr int kNormalSpeedIndex = 3;

// 播放按钮在空闲、失败或结束时可以从当前播放列表项重新开始。
bool canOpenSelectedSource(PlaybackState state)
{
    return state == PlaybackState::Idle || state == PlaybackState::Failed || state == PlaybackState::Ended;
}

// 定位只操作已打开、可定位且具有可用时长的当前会话。
bool canSeek(const PlaybackSnapshot& snapshot)
{
    return (snapshot.is_playing || snapshot.is_paused) && snapshot.capabilities.can_seek &&
           std::isfinite(snapshot.duration_seconds) && snapshot.duration_seconds > 0.0;
}

// 直播在打开、等待输出或播放时都可断开暂停；继续创建新的直播请求。
bool canTogglePlayback(const PlaybackSnapshot& snapshot)
{
    if (snapshot.source_kind == PlaybackSourceKind::NetworkLive) {
        return snapshot.state != PlaybackState::Idle && snapshot.state != PlaybackState::Stopping;
    }
    return canOpenSelectedSource(snapshot.state) ||
           (snapshot.source_kind == PlaybackSourceKind::LocalFile &&
            (snapshot.is_playing || snapshot.is_paused));
}

// 直播只使用固定标签，禁止将原地址、账号、查询参数填入任何展示控件。
QString rtspLabel(RtspTransport transport)
{
    return transport == RtspTransport::Udp ? "RTSP/UDP · 稳定" : "RTSP/TCP · 稳定";
}

QString playbackDisplayLabel(const PlaybackSnapshot& snapshot)
{
    if (snapshot.source_kind == PlaybackSourceKind::NetworkLive) {
        if (snapshot.protocol == PlaybackProtocol::Rtsp && snapshot.rtsp_transport) {
            return rtspLabel(*snapshot.rtsp_transport);
        }
        return "网络直播";
    }
    return QString::fromStdString(snapshot.display_name);
}

// 这些时刻相对当前连接；未采到的事件保留空值，不用 0 冒充已发生。
QString formatMilestone(const std::optional<double>& seconds)
{
    return seconds && std::isfinite(*seconds) && *seconds >= 0.0
        ? QString("%1 ms").arg(*seconds * 1000.0, 0, 'f', 0) : QString("—");
}

QString queueLimitLabel(PacketQueueLimit limit)
{
    switch (limit) {
        case PacketQueueLimit::Packets: return "包数";
        case PacketQueueLimit::PayloadBytes: return "payload 字节";
        case PacketQueueLimit::TimestampSpan: return "时间戳跨度";
        default: return "—";
    }
}

// 提示只使用固定错误分类，不拼接远端文本或原始网络地址。
QString recoveryFailureLabel(const PlaybackSnapshot& snapshot)
{
    if (snapshot.last_error) {
        if (const auto* read = std::get_if<MediaReadResult>(&*snapshot.last_error)) {
            if (read->status == MediaReadStatus::QueueFull) return read->stage == MediaReadStage::AudioQueue
                ? "音频队列达到限额" : "视频队列达到限额";
            if (read->status == MediaReadStatus::PacketTooLarge) return snapshot.network_stats &&
                snapshot.network_stats->preset == LivePreset::LowLatency
                ? "单包超过低延迟限额，请重新以稳定模式打开" : "单包超过稳定模式限额，不支持此负载";
            if (read->stage == MediaReadStage::AudioOutput) return "音频输出超时";
            if (read->stage == MediaReadStage::VideoOutput) return "视频输出超时";
            if (read->stage == MediaReadStage::InitialOutput) return "等待首次输出失败";
            if (read->stage == MediaReadStage::RecoveryBudget) return "恢复总时间已用尽";
            if (read->status == MediaReadStatus::Timeout) return "读取超时";
            if (read->status == MediaReadStatus::EndOfStream) return "直播结束或连接中断";
        }
        if (const auto* open = std::get_if<MediaOpenResult>(&*snapshot.last_error)) {
            if (open->status == MediaOpenStatus::Timeout) {
                return open->stage == MediaOpenStage::ProbeStreams ? "媒体探测超时" : "连接超时";
            }
        }
    }
    if (!snapshot.network_stats || !snapshot.network_stats->last_failure_reason) return "—";
    switch (*snapshot.network_stats->last_failure_reason) {
        case LiveFailureReason::AccessDenied: return "访问被拒绝";
        case LiveFailureReason::InvalidSource: return "地址或参数错误";
        case LiveFailureReason::UnsupportedInput: return "协议或模式不支持";
        case LiveFailureReason::DecoderUnavailable: return "解码器不可用";
        case LiveFailureReason::ResourceExhausted: return "资源不足";
        case LiveFailureReason::SetupFailed: return "媒体初始化失败";
        case LiveFailureReason::OutputTimeout: return "媒体输出失活";
        case LiveFailureReason::LiveEof: return "直播结束或连接中断";
        case LiveFailureReason::QueueLimit: return "队列达到限额";
        case LiveFailureReason::PacketTooLarge: return "单包过大";
        default: return "连接失败或中断";
    }
}

} // namespace

// ============================================================
// 构造函数
// ============================================================
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , current_playlist_index_(-1)
    , db_connected_(false)
{
    // 创建核心组件
    player_ = new PlayerController();
    database_ = new MediaDatabase();

    // 先创建停靠窗口（setupMenuBar需要引用它们）
    dock_playlist_ = new QDockWidget("播放列表", this);
    dock_info_ = new QDockWidget("媒体信息", this);

    // 构建UI
    setupUI();
    applyDarkTheme();
    connectSignals();

    // 启动信息更新定时器（200ms刷新一次播放信息）
    QObject::connect(&info_timer_, &QTimer::timeout, this, &MainWindow::updatePlaybackInfo);
    info_timer_.start(200);
    close_timer_.setInterval(16);
    QObject::connect(&close_timer_, &QTimer::timeout, this, &MainWindow::finishCloseWhenReady);

    setWindowTitle("MediaStudio - 音视频工作站");
    resize(1280, 720);
}

MainWindow::~MainWindow()
{
    close_state_ = CloseState::Ready;
    info_timer_.stop();
    close_timer_.stop();
    // 直接析构也先同时取消，再接回线程。VideoWidget 由 Qt 在此之后销毁。
    player_->requestShutdown();
    if (transcoder_) transcoder_->cancel();
    player_->shutdown();
    if (transcoding_thread_.joinable()) {
        transcoding_thread_.join();
    }
    delete player_;
    delete database_;
}

// ============================================================
// UI构建
// ============================================================
void MainWindow::setupUI()
{
    setupMenuBar();
    setupToolBar();
    setupCentralWidget();
    setupDockWidgets();
    setupBottomBar();
}

void MainWindow::setupMenuBar()
{
    // 文件菜单
    QMenu* menu_file = menuBar()->addMenu("文件");
    menu_file->addAction("打开文件(&O)", QKeySequence(), this, &MainWindow::openFile);
    menu_file->addAction("打开网络媒体…", this, &MainWindow::openNetworkMedia);
    menu_file->addAction("添加到播放列表", this, &MainWindow::addToPlaylist);
    menu_file->addSeparator();
    menu_file->addAction("退出(Esc)", QKeySequence(Qt::Key_Escape), this, &QWidget::close);

    // 视图菜单
    QMenu* menu_view = menuBar()->addMenu("视图");
    menu_view->addAction(dock_playlist_->toggleViewAction());
    menu_view->addAction(dock_info_->toggleViewAction());
    menu_view->addSeparator();
    menu_view->addAction("切换视频特效", this, &MainWindow::toggleEffect);
    menu_view->addAction("切换频谱可视化", this, &MainWindow::toggleVisualizer);

    // 工具菜单
    QMenu* menu_tools = menuBar()->addMenu("工具");
    menu_tools->addAction("连接数据库", this, &MainWindow::connectDatabaseDialog);
    menu_tools->addAction("格式转码", this, &MainWindow::transcodeDialog);

    // 帮助菜单
    QMenu* menu_help = menuBar()->addMenu("帮助");
    menu_help->addAction("快捷键说明", this, [this]() {
        if (isClosing()) return;
        QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
        QMessageBox::information(this, "快捷键说明",
            "SPACE - 播放/暂停（直播暂停断开，继续接入最新直播）\n"
            "LEFT/RIGHT - 本地文件后退/前进5秒\n"
            "UP/DOWN - 音量增大/减小\n"
            "PAGEUP/PAGEDOWN - 上一曲/下一曲\n"
            "ESC - 退出程序\n"
            "E - 切换视频特效\n"
            "V - 切换频谱可视化\n"
            "1/2/3/4 - 本地文件速度 0.5x/1.0x/1.5x/2.0x\n"
            "M - 静音/取消静音\n"
            "F - 切换收藏");
    });
    menu_help->addAction("关于", this, [this]() {
        if (isClosing()) return;
        QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
        QMessageBox::about(this, "关于 MediaStudio",
            "MediaStudio v1.0\n\n"
            "基于 FFmpeg + OpenGL + Qt 的音视频工作站\n\n"
            "技术栈:\n"
            "• FFmpeg 8.0 - 音视频编解码\n"
            "• OpenGL 3.3 - GPU渲染与特效\n"
            "• Qt 6.11 - 跨平台GUI框架\n"
            "• WASAPI - Windows音频播放\n"
            "• MySQL - 媒体库数据库");
    });
}

void MainWindow::setupToolBar()
{
    QToolBar* toolbar = addToolBar("主工具栏");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(24, 24));

    // 打开文件按钮
    QPushButton* btn_open = new QPushButton("📂 打开");
    btn_open->setToolTip("打开媒体文件");
    btn_open->setMinimumHeight(32);
    toolbar->addWidget(btn_open);
    QObject::connect(btn_open, &QPushButton::clicked, this, &MainWindow::openFile);

    toolbar->addSeparator();

    // 上一曲
    btn_prev_ = new QPushButton("◀◀");
    btn_prev_->setToolTip("上一曲");
    btn_prev_->setMinimumHeight(32);
    btn_prev_->setMaximumWidth(40);
    toolbar->addWidget(btn_prev_);

    // 播放/暂停
    btn_play_ = new QPushButton("▶");
    btn_play_->setToolTip("播放/暂停 (Space)");
    btn_play_->setFixedSize(50, 32);
    btn_play_->setStyleSheet("QPushButton { font-size: 16px; font-weight: bold; }");
    toolbar->addWidget(btn_play_);

    // 停止
    btn_stop_ = new QPushButton("■");
    btn_stop_->setToolTip("停止");
    btn_stop_->setMinimumHeight(32);
    btn_stop_->setMaximumWidth(40);
    toolbar->addWidget(btn_stop_);

    // 下一曲
    btn_next_ = new QPushButton("▶▶");
    btn_next_->setToolTip("下一曲");
    btn_next_->setMinimumHeight(32);
    btn_next_->setMaximumWidth(40);
    toolbar->addWidget(btn_next_);

    // 只在当前请求为直播时显示，不改变播放列表所选源的传输或预设。
    btn_live_ = new QPushButton("回到直播");
    btn_live_->setObjectName("btn_return_to_live");
    btn_live_->setMinimumHeight(32);
    btn_live_->setToolTip("重新连接当前直播，接入最新内容；连接中可再次点击或暂停。");
    action_live_ = toolbar->addWidget(btn_live_);
    action_live_->setVisible(false);

    toolbar->addSeparator();

    // 音量控制
    btn_mute_ = new QPushButton("🔊");
    btn_mute_->setToolTip("静音/取消静音 (M)");
    btn_mute_->setCheckable(true);
    btn_mute_->setMinimumHeight(32);
    btn_mute_->setMaximumWidth(40);
    btn_mute_->setObjectName("btn_mute");
    toolbar->addWidget(btn_mute_);

    slider_volume_ = new QSlider(Qt::Horizontal);
    slider_volume_->setRange(0, 100);
    slider_volume_->setValue(100);
    slider_volume_->setMaximumWidth(120);
    slider_volume_->setToolTip("音量控制");
    toolbar->addWidget(slider_volume_);

    toolbar->addSeparator();

    // 播放速度
    toolbar->addWidget(new QLabel(" 速度:"));
    combo_speed_ = new QComboBox();
    combo_speed_->addItems({"0.25x", "0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "2.0x", "4.0x"});
    combo_speed_->setCurrentIndex(local_speed_index_);
    combo_speed_->setMaximumWidth(80);
    toolbar->addWidget(combo_speed_);

    toolbar->addSeparator();

    // 特效按钮
    QPushButton* btn_effect = new QPushButton("🎨 特效");
    btn_effect->setToolTip("切换视频特效");
    btn_effect->setMinimumHeight(32);
    toolbar->addWidget(btn_effect);
    QObject::connect(btn_effect, &QPushButton::clicked, this, &MainWindow::toggleEffect);

    // 可视化按钮
    QPushButton* btn_viz = new QPushButton("🎵 频谱");
    btn_viz->setToolTip("切换音频频谱可视化");
    btn_viz->setMinimumHeight(32);
    toolbar->addWidget(btn_viz);
    QObject::connect(btn_viz, &QPushButton::clicked, this, &MainWindow::toggleVisualizer);
}

void MainWindow::setupCentralWidget()
{
    // 创建视频显示控件
    video_widget_ = new VideoWidget(this);
}

void MainWindow::setupDockWidgets()
{
    // ---- 播放列表停靠窗口 ----
    // dock_playlist_ 已在构造函数中创建
    dock_playlist_->setAllowedAreas(Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);

    QWidget* playlist_container = new QWidget();
    QVBoxLayout* playlist_layout = new QVBoxLayout(playlist_container);

    list_playlist_ = new QListWidget();
    list_playlist_->setAlternatingRowColors(true);
    playlist_layout->addWidget(list_playlist_);

    // 播放列表控制按钮
    QHBoxLayout* playlist_btn_layout = new QHBoxLayout();
    QPushButton* btn_add = new QPushButton("➕ 添加");
    QPushButton* btn_remove = new QPushButton("➖ 移除");
    QPushButton* btn_clear = new QPushButton("🗑 清空");
    playlist_btn_layout->addWidget(btn_add);
    playlist_btn_layout->addWidget(btn_remove);
    playlist_btn_layout->addWidget(btn_clear);
    playlist_layout->addLayout(playlist_btn_layout);

    QObject::connect(btn_add, &QPushButton::clicked, this, &MainWindow::addToPlaylist);
    QObject::connect(btn_remove, &QPushButton::clicked, this, &MainWindow::removeFromPlaylist);
    QObject::connect(btn_clear, &QPushButton::clicked, this, [this]() {
        if (isClosing()) return;
        list_playlist_->clear();
        playlist_.clear();
        current_playlist_index_ = -1;
    });
    QObject::connect(list_playlist_, &QListWidget::itemDoubleClicked,
        this, &MainWindow::playlistItemDoubleClicked);

    dock_playlist_->setWidget(playlist_container);
    addDockWidget(Qt::RightDockWidgetArea, dock_playlist_);

    // ---- 媒体信息停靠窗口 ----
    // dock_info_ 已在构造函数中创建
    dock_info_->setAllowedAreas(Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);

    QWidget* info_container = new QWidget();
    QVBoxLayout* info_layout = new QVBoxLayout(info_container);

    text_media_info_ = new QTextEdit();
    text_media_info_->setReadOnly(true);
    text_media_info_->setMinimumWidth(250);
    info_layout->addWidget(text_media_info_);

    dock_info_->setWidget(info_container);
    addDockWidget(Qt::RightDockWidgetArea, dock_info_);

    // 默认显示播放列表，隐藏信息面板
    dock_playlist_->show();
    dock_info_->hide();
}

void MainWindow::setupBottomBar()
{
    // 底部播放控制栏
    QWidget* bottom_bar = new QWidget();
    QVBoxLayout* bottom_layout = new QVBoxLayout(bottom_bar);
    bottom_layout->setContentsMargins(8, 4, 8, 4);

    // 进度条区域
    QHBoxLayout* progress_layout = new QHBoxLayout();

    label_time_ = new QLabel("00:00");
    label_time_->setMinimumWidth(50);
    label_time_->setAlignment(Qt::AlignCenter);

    slider_progress_ = new QSlider(Qt::Horizontal);
    slider_progress_->setRange(0, 1000);
    slider_progress_->setValue(0);
    slider_progress_->setEnabled(false);
    slider_progress_->setToolTip("点击或拖动跳转播放位置");

    label_duration_ = new QLabel("00:00");
    label_duration_->setMinimumWidth(50);
    label_duration_->setAlignment(Qt::AlignCenter);

    progress_layout->addWidget(label_time_);
    progress_layout->addWidget(slider_progress_, 1);
    progress_layout->addWidget(label_duration_);

    bottom_layout->addLayout(progress_layout);

    // 状态栏
    label_debug_stats_ = new QLabel("FPS -- | A/V -- ms | Drop -- | A -- / V -- | Buf -- ms");
    statusBar()->addPermanentWidget(label_debug_stats_);
    statusBar()->showMessage("就绪 - 打开文件或网络媒体开始播放");

    // 将视频和底部栏组合为中央部件
    QWidget* central_container = new QWidget();
    QVBoxLayout* main_layout = new QVBoxLayout(central_container);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // 视频控件
    main_layout->addWidget(video_widget_, 1);
    main_layout->addWidget(bottom_bar);

    setCentralWidget(central_container);
}

// ============================================================
// 暗色主题
// ============================================================
void MainWindow::applyDarkTheme()
{
    QString style = R"(
        QMainWindow {
            background-color: #1e1e2e;
        }
        QWidget {
            background-color: #1e1e2e;
            color: #cdd6f4;
            font-family: "Microsoft YaHei", "Segoe UI", sans-serif;
        }
        QPushButton {
            background-color: #313244;
            color: #cdd6f4;
            border: 1px solid #45475a;
            border-radius: 6px;
            padding: 6px 14px;
            font-size: 13px;
        }
        QPushButton:hover {
            background-color: #45475a;
            border-color: #89b4fa;
        }
        QPushButton:pressed {
            background-color: #585b70;
        }
        QPushButton#btn_mute:checked {
            background-color: #f38ba8;
            color: #11111b;
        }
        QSlider::groove:horizontal {
            background: #313244;
            height: 6px;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #89b4fa;
            width: 16px;
            height: 16px;
            margin: -5px 0;
            border-radius: 8px;
        }
        QSlider::handle:horizontal:hover {
            background: #b4befe;
        }
        QSlider::sub-page:horizontal {
            background: #89b4fa;
            border-radius: 3px;
        }
        QListWidget {
            background-color: #181825;
            border: 1px solid #313244;
            border-radius: 4px;
            font-size: 13px;
        }
        QListWidget::item {
            padding: 6px;
            border-bottom: 1px solid #313244;
        }
        QListWidget::item:selected {
            background-color: #45475a;
            color: #89b4fa;
        }
        QListWidget::item:alternate {
            background-color: #1e1e2e;
        }
        QTextEdit {
            background-color: #181825;
            border: 1px solid #313244;
            border-radius: 4px;
            font-size: 12px;
            padding: 8px;
        }
        QComboBox {
            background-color: #313244;
            border: 1px solid #45475a;
            border-radius: 4px;
            padding: 4px 8px;
            color: #cdd6f4;
        }
        QComboBox::drop-down {
            border: none;
        }
        QComboBox QAbstractItemView {
            background-color: #313244;
            color: #cdd6f4;
            selection-background-color: #45475a;
        }
        QDockWidget {
            color: #cdd6f4;
            titlebar-close-icon: none;
        }
        QDockWidget::title {
            background-color: #313244;
            padding: 6px;
            border-bottom: 1px solid #45475a;
        }
        QMenuBar {
            background-color: #181825;
            color: #cdd6f4;
            border-bottom: 1px solid #313244;
        }
        QMenuBar::item:selected {
            background-color: #45475a;
        }
        QMenu {
            background-color: #313244;
            color: #cdd6f4;
            border: 1px solid #45475a;
        }
        QMenu::item:selected {
            background-color: #585b70;
        }
        QToolBar {
            background-color: #181825;
            border-bottom: 1px solid #313244;
            spacing: 4px;
            padding: 4px;
        }
        QStatusBar {
            background-color: #181825;
            color: #a6adc8;
            border-top: 1px solid #313244;
        }
        QLabel {
            background: transparent;
        }
    )";
    setStyleSheet(style);
}

// ============================================================
// 信号连接
// ============================================================
void MainWindow::connectSignals()
{
    // 将PlayerController与VideoWidget关联
    player_->setVideoWidget(video_widget_);

    // 播放控制按钮
    QObject::connect(btn_play_, &QPushButton::clicked, this, &MainWindow::togglePlayPause);
    QObject::connect(btn_stop_, &QPushButton::clicked, this, &MainWindow::stopPlayback);
    QObject::connect(btn_prev_, &QPushButton::clicked, this, &MainWindow::prevFile);
    QObject::connect(btn_next_, &QPushButton::clicked, this, &MainWindow::nextFile);
    QObject::connect(btn_live_, &QPushButton::clicked, this, &MainWindow::returnToLive);
    QObject::connect(btn_mute_, &QPushButton::clicked, this, &MainWindow::toggleMute);

    // 进度条
    QObject::connect(slider_progress_, &QSlider::sliderMoved, this, &MainWindow::seekPosition);

    // 音量
    QObject::connect(slider_volume_, &QSlider::valueChanged, this, &MainWindow::changeVolume);

    // 速度
    QObject::connect(combo_speed_, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &MainWindow::changeSpeed);

    // 键盘快捷键
    setupShortcuts();
}

void MainWindow::setupShortcuts()
{
    // 播放/暂停 (Space)
    QShortcut* sc_space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    sc_space->setContext(Qt::ApplicationShortcut);
    connect(sc_space, &QShortcut::activated, this, &MainWindow::togglePlayPause);

    // 快进 (Right)
    QShortcut* sc_right = new QShortcut(QKeySequence(Qt::Key_Right), this);
    sc_right->setContext(Qt::ApplicationShortcut);
    connect(sc_right, &QShortcut::activated, this, [this]() {
        seekRelative(5.0);
    });

    // 快退 (Left)
    QShortcut* sc_left = new QShortcut(QKeySequence(Qt::Key_Left), this);
    sc_left->setContext(Qt::ApplicationShortcut);
    connect(sc_left, &QShortcut::activated, this, [this]() {
        seekRelative(-5.0);
    });

    // 音量加 (Up)
    QShortcut* sc_up = new QShortcut(QKeySequence(Qt::Key_Up), this);
    sc_up->setContext(Qt::ApplicationShortcut);
    connect(sc_up, &QShortcut::activated, this, [this]() {
        slider_volume_->setValue(slider_volume_->value() + 5);
    });

    // 音量减 (Down)
    QShortcut* sc_down = new QShortcut(QKeySequence(Qt::Key_Down), this);
    sc_down->setContext(Qt::ApplicationShortcut);
    connect(sc_down, &QShortcut::activated, this, [this]() {
        slider_volume_->setValue(slider_volume_->value() - 5);
    });

    // 下一个 (PageDown)
    QShortcut* sc_next = new QShortcut(QKeySequence(Qt::Key_PageDown), this);
    sc_next->setContext(Qt::ApplicationShortcut);
    connect(sc_next, &QShortcut::activated, this, &MainWindow::nextFile);

    // 上一个 (PageUp)
    QShortcut* sc_prev = new QShortcut(QKeySequence(Qt::Key_PageUp), this);
    sc_prev->setContext(Qt::ApplicationShortcut);
    connect(sc_prev, &QShortcut::activated, this, &MainWindow::prevFile);

    // 倍速 (1-4)
    QShortcut* sc_speed1 = new QShortcut(QKeySequence(Qt::Key_1), this);
    sc_speed1->setContext(Qt::ApplicationShortcut);
    connect(sc_speed1, &QShortcut::activated, this, [this]() {
        if (combo_speed_->isEnabled()) combo_speed_->setCurrentIndex(1);
    });

    QShortcut* sc_speed2 = new QShortcut(QKeySequence(Qt::Key_2), this);
    sc_speed2->setContext(Qt::ApplicationShortcut);
    connect(sc_speed2, &QShortcut::activated, this, [this]() {
        if (combo_speed_->isEnabled()) combo_speed_->setCurrentIndex(3);
    });

    QShortcut* sc_speed3 = new QShortcut(QKeySequence(Qt::Key_3), this);
    sc_speed3->setContext(Qt::ApplicationShortcut);
    connect(sc_speed3, &QShortcut::activated, this, [this]() {
        if (combo_speed_->isEnabled()) combo_speed_->setCurrentIndex(5);
    });

    QShortcut* sc_speed4 = new QShortcut(QKeySequence(Qt::Key_4), this);
    sc_speed4->setContext(Qt::ApplicationShortcut);
    connect(sc_speed4, &QShortcut::activated, this, [this]() {
        if (combo_speed_->isEnabled()) combo_speed_->setCurrentIndex(6);
    });

    // 收藏 (F)
    QShortcut* sc_fav = new QShortcut(QKeySequence(Qt::Key_F), this);
    sc_fav->setContext(Qt::ApplicationShortcut);
    connect(sc_fav, &QShortcut::activated, this, [this]() {
        if (!isClosing() && db_connected_) {
            auto snapshot = player_->getPlaybackSnapshot();
            auto& info = snapshot.media_info;
            if (snapshot.request_id == playback_request_id_ &&
                snapshot.source_kind == PlaybackSourceKind::LocalFile && info.db_id > 0) {
                bool new_state = !info.is_favorite;
                if (database_->setFavorite(info.db_id, new_state)) {
                    info.is_favorite = new_state;
                    // 数据库操作完成时可能已经切源，只回写这次读取的本地源。
                    if (player_->updateLocalLibraryInfo(snapshot.request_id, info)) {
                        statusBar()->showMessage(new_state ? "已添加收藏" : "已取消收藏");
                    }
                }
            }
        }
    });

    // 静音 (M)
    QShortcut* sc_mute = new QShortcut(QKeySequence(Qt::Key_M), this);
    sc_mute->setContext(Qt::ApplicationShortcut);
    connect(sc_mute, &QShortcut::activated, this, &MainWindow::toggleMute);

    // 切换视频特效 (E)
    QShortcut* sc_effect = new QShortcut(QKeySequence(Qt::Key_E), this);
    sc_effect->setContext(Qt::ApplicationShortcut);
    connect(sc_effect, &QShortcut::activated, this, &MainWindow::toggleEffect);

    // 切换频谱可视化 (V)
    QShortcut* sc_viz = new QShortcut(QKeySequence(Qt::Key_V), this);
    sc_viz->setContext(Qt::ApplicationShortcut);
    connect(sc_viz, &QShortcut::activated, this, &MainWindow::toggleVisualizer);
}

// ============================================================
// 槽函数实现
// ============================================================
void MainWindow::openFile()
{
    if (isClosing()) return;
    QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
    QString filename = QFileDialog::getOpenFileName(this,
        "打开媒体文件", "",
        "媒体文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv *.mp3 *.wav);;所有文件 (*.*)");

    if (isClosing() || filename.isEmpty()) return;

    const int row = appendPlaylistEntry(PlaybackSource::localFile(filename.toStdString()));
    if (row >= 0) openPlaylistEntry(row);
    else statusBar()->showMessage("请选择本地文件；RTSP 地址请使用“打开网络媒体”。");
}

// 输入框以密码模式保存完整连接地址。只有成功校验的源对象离开对话框。
void MainWindow::openNetworkMedia()
{
    if (isClosing()) return;
    QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
    QDialog dialog(this);
    dialog.setObjectName("networkMediaDialog");
    dialog.setWindowTitle("打开网络媒体");

    auto* form = new QFormLayout(&dialog);
    auto* url_input = new QLineEdit(&dialog);
    url_input->setObjectName("networkUrlInput");
    url_input->setMinimumWidth(350);
    url_input->setPlaceholderText("rtsp://主机:端口/流名");
    url_input->setEchoMode(QLineEdit::Password);
    url_input->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    form->addRow("RTSP 地址：", url_input);

    auto* transport = new QComboBox(&dialog);
    transport->setObjectName("rtspTransportSelector");
    transport->addItem("TCP", static_cast<int>(RtspTransport::Tcp));
    transport->addItem("UDP", static_cast<int>(RtspTransport::Udp));
    form->addRow("传输方式：", transport);
    form->addRow("播放模式：", new QLabel("稳定", &dialog));
    auto* automatic = new QCheckBox("自动重连", &dialog);
    automatic->setObjectName("autoReconnectOption");
    automatic->setChecked(true);
    automatic->setToolTip("断流后最多重试 3 次；暂停、停止和切源会取消恢复。");
    form->addRow(automatic);

    auto* error_label = new QLabel(&dialog);
    error_label->setObjectName("networkUrlError");
    error_label->setWordWrap(true);
    form->addRow(error_label);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName("networkDialogButtons");
    buttons->button(QDialogButtonBox::Open)->setText("打开");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    form->addRow(buttons);

    std::optional<PlaybackSource> selected;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (isClosing()) { dialog.reject(); return; }
        const auto selected_transport = static_cast<RtspTransport>(transport->currentData().toInt());
        auto result = createStableRtspSource(url_input->text().toStdString(), selected_transport, automatic->isChecked());
        if (!result.source) {
            error_label->setText(result.error == RtspSourceError::UnsupportedProtocol
                ? "当前版本仅支持 RTSP 直播地址。" : "请输入有效的 RTSP 地址。");
            url_input->setFocus();
            return;
        }
        selected = std::move(result.source);
        url_input->setText(QString{}); // 同时清除输入框的编辑历史。
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(&dialog, &QDialog::finished, url_input, [url_input] { url_input->setText(QString{}); });
    connect(url_input, &QLineEdit::textEdited, error_label, &QLabel::clear);
    url_input->setFocus();

    if (dialog.exec() != QDialog::Accepted || isClosing() || !selected) return;
    openPlaylistEntry(appendPlaylistEntry(std::move(*selected)));
}

// 根据当前请求状态决定播放、暂停或从列表重新打开。
void MainWindow::togglePlayPause()
{
    if (isClosing()) return;
    const auto snapshot = player_->getPlaybackSnapshot();
    if (snapshot.request_id != playback_request_id_ || !canTogglePlayback(snapshot)) return;
    if (snapshot.source_kind == PlaybackSourceKind::NetworkLive && canOpenSelectedSource(snapshot.state)) {
        returnToLive();
        return;
    }
    if (snapshot.source_kind == PlaybackSourceKind::NetworkLive || snapshot.is_playing || snapshot.is_paused) {
        // 控制器保存当前源；不根据用户单击选中的另一行决定继续目标。
        const auto request = player_->togglePlayPause();
        if (request == 0) return;
        playback_request_id_ = request;
        if (snapshot.source_kind == PlaybackSourceKind::NetworkLive) library_request_id_ = 0;
        if (current_playlist_index_ >= 0) list_playlist_->setCurrentRow(current_playlist_index_);
        updatePlaybackInfo();
    } else if (canOpenSelectedSource(snapshot.state)) {
        if (playlist_.empty()) {
            openFile();
        } else {
            openPlaylistEntry(std::max(0, current_playlist_index_));
        }
    }
}

// 用户主动恢复保留当前直播设置。重复点击由控制器撤销前一次连接，最新命令优先。
void MainWindow::returnToLive()
{
    if (isClosing()) return;
    const auto snapshot = player_->getPlaybackSnapshot();
    if (snapshot.request_id != playback_request_id_ || snapshot.source_kind != PlaybackSourceKind::NetworkLive ||
        !snapshot.capabilities.can_return_to_live || snapshot.state == PlaybackState::Idle ||
        snapshot.state == PlaybackState::Stopping) return;
    const auto request = player_->returnToLive();
    if (request == 0) return;
    playback_request_id_ = request;
    library_request_id_ = 0;
    if (current_playlist_index_ >= 0) list_playlist_->setCurrentRow(current_playlist_index_);
    updatePlaybackInfo();
}

// 取消尚未登记的库记录，画面与会话收尾由控制器管理。
void MainWindow::stopPlayback()
{
    if (isClosing()) return;
    library_request_id_ = 0;
    playback_request_id_ = player_->stopAsync();
    updatePlaybackInfo();
}

void MainWindow::seekPosition(int position)
{
    if (isClosing()) return;
    const auto snapshot = player_->getPlaybackSnapshot();
    if (snapshot.request_id != playback_request_id_ || !canSeek(snapshot)) return;
    player_->seek((position / 1000.0) * snapshot.duration_seconds);
}

// 快捷键按与进度条相同的能力判断，并使用同一快照的当前位置。
void MainWindow::seekRelative(double offset)
{
    if (isClosing()) return;
    const auto snapshot = player_->getPlaybackSnapshot();
    if (snapshot.request_id != playback_request_id_ || !canSeek(snapshot)) return;
    player_->seek(snapshot.current_seconds + offset);
}

void MainWindow::changeVolume(int volume)
{
    if (isClosing()) return;
    player_->setVolume(volume / 100.0f);
    is_muted_ = (volume == 0);
    if (!is_muted_) {
        last_volume_ = volume;
    }
    
    // 同步更新静音按钮状态
    btn_mute_->blockSignals(true);
    btn_mute_->setChecked(is_muted_);
    btn_mute_->setText(is_muted_ ? "🔇" : "🔊");
    btn_mute_->blockSignals(false);

    statusBar()->showMessage(QString("音量: %1%").arg(volume));
}

void MainWindow::changeSpeed(int index)
{
    if (isClosing() || index < 0 || index >= static_cast<int>(kPlaybackSpeeds.size())) return;
    const auto snapshot = player_->getPlaybackSnapshot();
    if (snapshot.request_id != playback_request_id_ || snapshot.source_kind != PlaybackSourceKind::LocalFile) return;
    local_speed_index_ = index;
    player_->setSpeed(kPlaybackSpeeds[index]);
    statusBar()->showMessage(QString("播放速度: %1").arg(combo_speed_->itemText(index)));
}

void MainWindow::nextFile()
{
    if (isClosing() || playlist_.empty()) return;
    openPlaylistEntry((current_playlist_index_ + 1) % static_cast<int>(playlist_.size()));
}

void MainWindow::prevFile()
{
    if (isClosing() || playlist_.empty()) return;
    const int count = static_cast<int>(playlist_.size());
    const int row = current_playlist_index_ < 0 ? count - 1 : (current_playlist_index_ - 1 + count) % count;
    openPlaylistEntry(row);
}

void MainWindow::toggleEffect()
{
    if (isClosing()) return;
    VideoEffect current = player_->getEffect();
    int next = (static_cast<int>(current) + 1) % static_cast<int>(VideoEffect::Count);
    VideoEffect new_effect = static_cast<VideoEffect>(next);
    player_->setEffect(new_effect);
    video_widget_->setEffect(new_effect);
    statusBar()->showMessage(QString("视频特效: %1").arg(
        QString::fromStdString(videoEffectToString(new_effect))));
}

void MainWindow::toggleVisualizer()
{
    if (isClosing()) return;
    player_->toggleVisualizer();
    bool showing = player_->isVisualizerShown();
    video_widget_->setShowSpectrum(showing);
    statusBar()->showMessage(showing ? "频谱可视化模式" : "视频播放模式");
}

void MainWindow::connectDatabaseDialog()
{
    if (isClosing()) return;
    QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
    bool ok;
    QString host = QInputDialog::getText(this, "数据库连接", "主机地址:",
        QLineEdit::Normal, "127.0.0.1", &ok);
    if (isClosing() || !ok) return;

    QString user = QInputDialog::getText(this, "数据库连接", "用户名:",
        QLineEdit::Normal, "root", &ok);
    if (isClosing() || !ok) return;

    QString password = QInputDialog::getText(this, "数据库连接", "密码:",
        QLineEdit::Password, "", &ok);
    if (isClosing() || !ok) return;

    QString database = QInputDialog::getText(this, "数据库连接", "数据库名:",
        QLineEdit::Normal, "media_center", &ok);
    if (isClosing() || !ok) return;

    if (connectDatabase(host.toStdString(), user.toStdString(),
                        password.toStdString(), database.toStdString())) {
        db_connected_ = true;
        QMessageBox::information(this, "成功", "数据库连接成功！");
    }
    else {
        QMessageBox::warning(this, "失败",
            "数据库连接失败:\n" + QString::fromStdString(database_->getLastError()));
    }
}

void MainWindow::transcodeDialog()
{
    if (isClosing()) return;
    QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
    if (transcoding_.load()) {
        QMessageBox::information(this, "提示", "已有转码任务正在执行，请稍候。");
        return;
    }

    QString input = QFileDialog::getOpenFileName(this, "选择源文件", "",
        "媒体文件 (*.mp4 *.mkv *.avi *.mov *.flv);;所有文件 (*.*)");
    if (isClosing() || input.isEmpty()) return;
    if (!PlaybackSource::localFile(input.toStdString()).isValid()) {
        statusBar()->showMessage("转码源请选择本地文件。");
        return;
    }

    QString output = QFileDialog::getSaveFileName(this, "保存转码文件", "",
        "MP4 (*.mp4);;MKV (*.mkv);;AVI (*.avi)");
    if (isClosing() || output.isEmpty()) return;
    if (!PlaybackSource::localFile(output.toStdString()).isValid()) {
        statusBar()->showMessage("转码目标请选择本地文件。");
        return;
    }
    startTranscode(input, output);
}

// 每项转码独立持有取消标志。完成标志由工作线程发布，关闭流程不依赖界面回调。
void MainWindow::startTranscode(const QString& input, const QString& output)
{
    if (isClosing() || transcoding_.load()) return;
    if (transcoding_thread_.joinable()) {
        transcoding_thread_.join();
    }

    transcoder_ = std::make_unique<MediaTranscoder>();
    auto* job = transcoder_.get();
    const auto request = ++transcode_request_id_;
    transcoding_ = true;
    statusBar()->showMessage("正在转码...");
    try {
        transcoding_thread_ = std::thread([this, job, request, input, output]() {
            bool success = false;
            QString detail;
            try {
                success = job->transcode(input.toStdString(), output.toStdString(), 0, 0, "libx264", "aac", 0,
                    [this, request](float progress) {
                        QMetaObject::invokeMethod(this, [this, request, progress]() {
                            if (isClosing() || request != transcode_request_id_) return;
                            statusBar()->showMessage(QString("转码进度: %1%").arg((int)(progress * 100)));
                        }, Qt::QueuedConnection);
                    });
                if (!success) detail = QString::fromStdString(job->getLastError());
            } catch (const std::exception& error) {
                detail = QString::fromUtf8(error.what());
            }

            // 回调以值捕获错误。旧任务迟到或关窗时，不再更新界面或打开消息框。
            QMetaObject::invokeMethod(this, [this, request, success, output, detail]() {
                if (isClosing() || request != transcode_request_id_) return;
                QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
                if (success) {
                    statusBar()->showMessage("转码完成: " + output);
                    QMessageBox::information(this, "完成", "转码完成！");
                } else {
                    statusBar()->showMessage("转码失败");
                    QMessageBox::warning(this, "失败", "转码失败:\n" + detail);
                }
            }, Qt::QueuedConnection);
            transcoding_ = false;
        });
    } catch (const std::system_error& error) {
        transcoding_ = false;
        statusBar()->showMessage("无法启动转码任务: " + QString::fromUtf8(error.what()));
    }
}

void MainWindow::addToPlaylist()
{
    if (isClosing()) return;
    QScopedValueRollback modal_scope(modal_depth_, modal_depth_ + 1);
    QStringList files = QFileDialog::getOpenFileNames(this, "添加文件到播放列表", "",
        "媒体文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv *.mp3 *.wav);;所有文件 (*.*)");
    if (isClosing()) return;

    bool rejected = false;
    for (const QString& file : files) {
        if (appendPlaylistEntry(PlaybackSource::localFile(file.toStdString())) < 0) rejected = true;
    }
    if (rejected) statusBar()->showMessage("非本地地址未添加；RTSP 请使用“打开网络媒体”。");
}

void MainWindow::removeFromPlaylist()
{
    if (isClosing()) return;
    int row = list_playlist_->currentRow();
    if (row < 0 || row >= static_cast<int>(playlist_.size())) return;

    delete list_playlist_->takeItem(row);
    playlist_.erase(playlist_.begin() + row);
    if (row < current_playlist_index_) {
        --current_playlist_index_;
    } else if (row == current_playlist_index_) {
        // 已移除的活动项仍可由控制器暂停/继续，不把下一行误当成它。
        current_playlist_index_ = -1;
    }
}

void MainWindow::playlistItemDoubleClicked(QListWidgetItem* item)
{
    if (isClosing()) return;
    openPlaylistEntry(list_playlist_->row(item));
}

// 只读取一次快照。旧请求即使完成，也不更新界面或登记播放历史。
void MainWindow::updatePlaybackInfo()
{
    if (isClosing()) return;
    const auto snapshot = player_->getPlaybackSnapshot();
    if (snapshot.request_id != playback_request_id_) return;
    updatePlaybackControls(snapshot);
    updateMediaInfoPanel(snapshot);
    syncLocalLibrary(snapshot);
}

// 文件显示容器、总时长；直播展示协议与轨道信息，不读取连接地址或文件字段。
void MainWindow::updateMediaInfoPanel(const PlaybackSnapshot& snapshot)
{
    const auto& info = snapshot.media_info;
    const bool live = snapshot.source_kind == PlaybackSourceKind::NetworkLive;
    QString info_text;
    if (live) {
        info_text += QString("<h3>%1</h3><p>类型: 直播</p>").arg(playbackDisplayLabel(snapshot).toHtmlEscaped());
        if (snapshot.state == PlaybackState::Paused) {
            info_text += snapshot.is_session_active ? "<p>正在暂停，停止输出并断开连接中。</p>"
                : "<p>已断开直播。继续时重新接入最新内容。</p>";
        }
        const auto stats = snapshot.network_stats.value_or(NetworkPlaybackStats{});
        info_text += QString("<p>连接尝试: %1 · 自动重试累计: %2<br>本轮已用: %3/3 · 自动重连: %4</p>")
            .arg(stats.reconnect_attempts + 1).arg(stats.reconnect_attempts).arg(stats.recovery_retries_used)
            .arg(stats.auto_reconnect_enabled ? "开" : "关");
        if (stats.last_failure_reason) info_text += QString("<p>最近故障: %1</p>").arg(recoveryFailureLabel(snapshot));
        info_text += QString("<p>队列 Full 累计: %1 · 超大单包: %2</p>")
            .arg(stats.queue_full_events).arg(stats.oversized_packet_events);
        info_text += QString("<p>本连接音频时间戳重建: %1</p>").arg(stats.audio_clock_reanchors);
        if (stats.last_queue_limit) info_text += QString("<p>最近触发限界: %1</p>").arg(queueLimitLabel(*stats.last_queue_limit));
        const auto queue_info = [](const QString& name, const PacketQueueStats& queue) {
            const auto span = queue.timestamp_span_seconds ? QString("%1 ms").arg(*queue.timestamp_span_seconds * 1000.0, 0, 'f', 0)
                : QString("不可用");
            return QString("<p><b>%1队列</b><br>包数: %2/%3 · payload: %4/%5 KiB<br>跨度: %6 / %7 ms"
                           "<br>峰值: %8 包 · %9 KiB</p>")
                .arg(name).arg(queue.packets).arg(queue.limits.packets)
                .arg(queue.payload_bytes / 1024.0, 0, 'f', 1).arg(queue.limits.payload_bytes / 1024.0, 0, 'f', 0)
                .arg(span).arg(std::chrono::duration<double, std::milli>(queue.limits.timestamp_span).count(), 0, 'f', 0)
                .arg(queue.peak_packets).arg(queue.peak_payload_bytes / 1024.0, 0, 'f', 1);
        };
        if (info.has_audio) info_text += queue_info("音频", stats.audio_queue);
        if (info.has_video) info_text += queue_info("视频", stats.video_queue);
        if (snapshot.state == PlaybackState::Reconnecting) info_text += "<p>旧连接及声音已释放，正在等待重试。</p>";
        // 视频就绪以有效帧提交为准；频谱可隐藏画面，因此绘制有独立时刻。
        if (info.has_audio || info.has_video) {
            // 暂停或失败时首次事件仅是历史，轨道状态以会话是否已收尾为准。
            const char* stopped_output = snapshot.state == PlaybackState::Reconnecting ? "已停止输出" :
                snapshot.state == PlaybackState::Paused || snapshot.state == PlaybackState::Failed
                ? (snapshot.is_session_active ? "停止输出中" : "已停止输出") : nullptr;
            const auto audio_status = !info.has_audio ? "无轨道" : stopped_output ? stopped_output
                : snapshot.state == PlaybackState::Buffering && stats.last_audio_output_age_seconds.value_or(0.0) > 1.0
                    ? "输出中断，等待恢复"
                : stats.first_pcm_seconds ? "已提交 PCM" : "待输出";
            const auto video_status = !info.has_video ? "无轨道" : stopped_output ? stopped_output
                : snapshot.state == PlaybackState::Buffering && stats.last_video_output_age_seconds.value_or(0.0) > 1.0
                    ? "输出中断，等待恢复"
                : stats.first_video_submit_seconds ? "已提交视频帧" : "待输出";
            info_text += QString("<p>音频: %1<br>视频: %2</p>").arg(audio_status, video_status);
        }
        info_text += QString("<p><b>首次事件（从本次连接计时）</b><br>收包: %1 · PCM: %2<br>视频提交: %3 · 绘制: %4"
                             "<br>损坏帧跳过: %5</p>")
            .arg(formatMilestone(stats.first_packet_seconds), formatMilestone(stats.first_pcm_seconds),
                 formatMilestone(stats.first_video_submit_seconds), formatMilestone(stats.first_video_draw_seconds))
            .arg(stats.corrupt_frames_skipped);
        info_text += QString("<p>探测完成: %1<br>距上次音频输出: %2<br>距上次视频提交: %3</p>")
            .arg(formatMilestone(stats.input_ready_seconds), formatMilestone(stats.last_audio_output_age_seconds),
                 formatMilestone(stats.last_video_output_age_seconds));
    } else if (!info.file_name.empty()) {
        info_text += QString("<h3>%1</h3>").arg(QString::fromStdString(info.file_name).toHtmlEscaped());
        info_text += QString("<p>格式: %1</p>").arg(QString::fromStdString(info.format_name).toHtmlEscaped());
        info_text += QString("<p>时长: %1秒</p>").arg(info.duration, 0, 'f', 1);
    }

    if (info.has_video) {
        info_text += "<hr><h4>视频</h4>";
        info_text += QString("<p>编码: %1</p>").arg(QString::fromStdString(info.video_info.codec_name).toHtmlEscaped());
        info_text += QString("<p>分辨率: %1x%2</p>").arg(info.video_info.width).arg(info.video_info.height);
        info_text += QString("<p>帧率: %1 fps</p>").arg(info.video_info.frame_rate, 0, 'f', 1);
        info_text += QString("<p>特效: %1</p>").arg(
            QString::fromStdString(videoEffectToString(player_->getEffect())));
    }

    if (info.has_audio) {
        info_text += "<hr><h4>音频</h4>";
        info_text += QString("<p>编码: %1</p>").arg(QString::fromStdString(info.audio_info.codec_name).toHtmlEscaped());
        info_text += QString("<p>采样率: %1 Hz</p>").arg(info.audio_info.sample_rate);
        info_text += QString("<p>声道: %1</p>").arg(info.audio_info.channels);
    }
    if (info_text != last_media_info_html_) {
        int scroll_value = text_media_info_->verticalScrollBar()->value();
        text_media_info_->setHtml(info_text);
        text_media_info_->verticalScrollBar()->setValue(scroll_value);
        last_media_info_html_ = info_text;
    }
}

// 根据同一快照刷新播放状态、定位能力、进度和实时统计。
void MainWindow::updatePlaybackControls(const PlaybackSnapshot& snapshot)
{
    const bool live = snapshot.source_kind == PlaybackSourceKind::NetworkLive;
    const bool live_can_pause = live && snapshot.state != PlaybackState::Paused &&
        !canOpenSelectedSource(snapshot.state) && snapshot.state != PlaybackState::Stopping;
    btn_play_->setText(live_can_pause || (snapshot.is_playing && !snapshot.is_paused) ? "⏸" : "▶");
    btn_play_->setEnabled(canTogglePlayback(snapshot));
    btn_play_->setToolTip(!live ? "播放/暂停 (Space)" : live_can_pause ? "暂停直播并断开连接 (Space)"
        : snapshot.state == PlaybackState::Paused ? "继续直播，重新接入最新内容 (Space)" : "重新连接当前直播 (Space)");
    action_live_->setVisible(live);
    action_live_->setEnabled(live && snapshot.capabilities.can_return_to_live &&
        snapshot.state != PlaybackState::Idle && snapshot.state != PlaybackState::Stopping);
    const bool seekable = canSeek(snapshot);
    slider_progress_->setEnabled(seekable);
    slider_progress_->setToolTip(live ? "直播不可定位" : "点击或拖动跳转播放位置");
    combo_speed_->setEnabled(!live && (snapshot.source_kind == PlaybackSourceKind::LocalFile ||
                                       snapshot.capabilities.can_change_speed));
    // 直播的固定 1.0x 仅改变显示，不发送速度命令或覆盖文件偏好。
    const int visible_speed = live ? kNormalSpeedIndex : local_speed_index_;
    if (combo_speed_->currentIndex() != visible_speed) {
        const QSignalBlocker blocker(combo_speed_);
        combo_speed_->setCurrentIndex(visible_speed);
    }

    // 状态变化时才写状态栏，保留音量、倍速或特效操作出现时的提示。
    if (snapshot.request_id != displayed_request_id_ || snapshot.state != displayed_state_ ||
        (live && snapshot.state == PlaybackState::Paused && snapshot.is_session_active != displayed_session_active_) ||
        (live && snapshot.state == PlaybackState::Reconnecting)) {
        const QString name = playbackDisplayLabel(snapshot);
        QString status;
        switch (snapshot.state) {
            case PlaybackState::Idle: status = "已停止"; break;
            case PlaybackState::Opening: status = "正在加载: " + name; break;
            case PlaybackState::Buffering: status = (live ? "正在缓冲，等待轨道输出: " : "正在缓冲: ") + name; break;
            case PlaybackState::Playing: status = "正在播放: " + name; break;
            case PlaybackState::Draining: status = "正在播放: " + name; break;
            case PlaybackState::Paused:
                status = !live ? "已暂停" : snapshot.is_session_active
                    ? "正在暂停，断开连接中: " + name : "已暂停（已断开直播）: " + name;
                break;
            case PlaybackState::Reconnecting:
                status = QString("正在重新连接: %1（约 %2 秒后重试）").arg(name)
                    .arg(snapshot.network_stats ? snapshot.network_stats->retry_in_seconds.value_or(0.0) : 0.0, 0, 'f', 1);
                break;
            case PlaybackState::Stopping: status = "正在停止"; break;
            case PlaybackState::Failed: {
                status = "播放失败: " + name;
                if (live && snapshot.last_error) {
                    const auto* read = std::get_if<MediaReadResult>(&*snapshot.last_error);
                    if (read && read->stage == MediaReadStage::InitialOutput) {
                        status = (read->status == MediaReadStatus::Timeout
                            ? "播放失败，等待轨道输出超时: " : "播放失败，无法启动轨道输出: ") + name;
                    }
                    if (read && (read->stage == MediaReadStage::AudioOutput || read->stage == MediaReadStage::VideoOutput)) {
                        status = QString("播放失败，%1输出超时: ")
                            .arg(read->stage == MediaReadStage::AudioOutput ? "音频" : "视频") + name;
                    }
                    const auto stopped = snapshot.network_stats ? snapshot.network_stats->recovery_stop_reason : std::nullopt;
                    if (stopped == RecoveryStopReason::AttemptsExhausted) status = "播放失败，重试次数已用尽: " + name;
                    if (stopped == RecoveryStopReason::DeadlineExceeded) status = "播放失败，恢复总时间已用尽: " + name;
                    if (read && read->status == MediaReadStatus::PacketTooLarge) {
                        status = "播放失败，" + recoveryFailureLabel(snapshot) + ": " + name;
                    }
                    if (read && read->status == MediaReadStatus::QueueFull && stopped == RecoveryStopReason::Disabled) {
                        status = "播放失败，队列达到限额（自动重连已关闭）: " + name;
                    }
                }
                break;
            }
            case PlaybackState::Ended: status = "播放结束: " + name; break;
        }
        statusBar()->showMessage(status);
        displayed_request_id_ = snapshot.request_id;
        displayed_state_ = snapshot.state;
        displayed_session_active_ = snapshot.is_session_active;
    }

    const auto formatTime = [](double seconds) {
        const auto total = static_cast<qint64>(seconds);
        return QString("%1:%2").arg(total / 60, 2, 10, QChar('0')).arg(total % 60, 2, 10, QChar('0'));
    };
    const double current = std::isfinite(snapshot.current_seconds) ? std::max(0.0, snapshot.current_seconds) : 0.0;
    const double duration = snapshot.duration_seconds;
    const bool known_duration = !live && snapshot.capabilities.duration_known && std::isfinite(duration) && duration > 0.0;
    if (live) {
        const auto elapsed = snapshot.live_elapsed_seconds;
        label_time_->setText(QString("已播放 %1").arg(elapsed && std::isfinite(*elapsed)
            ? formatTime(std::max(0.0, *elapsed)) : QString("--:--")));
    } else {
        label_time_->setText(formatTime(current));
    }
    if (!slider_progress_->isSliderDown() || !seekable) {
        const double ratio = known_duration ? std::clamp(current / duration, 0.0, 1.0) : 0.0;
        slider_progress_->setValue(static_cast<int>(ratio * 1000));
    }
    if (live) {
        label_duration_->setText("直播");
    } else if (known_duration) {
        label_duration_->setText(formatTime(duration));
    } else {
        label_duration_->setText(snapshot.state == PlaybackState::Idle ? "00:00" : "--:--");
    }

    if (snapshot.is_playing || snapshot.is_paused) {
        const auto& stats = snapshot.stats;
        label_debug_stats_->setText(QString("FPS %1/%2 | A/V %3 ms | Drop %4 | A %5 / V %6 | Buf %7 ms")
            .arg(stats.render_fps, 0, 'f', 1)
            .arg(stats.decode_fps, 0, 'f', 1)
            .arg(stats.av_diff_ms, 0, 'f', 0)
            .arg(stats.dropped_frames_total)
            .arg(stats.audio_queue_packets)
            .arg(stats.video_queue_packets)
            .arg(stats.audio_buffer_seconds * 1000.0, 0, 'f', 0));
    } else {
        label_debug_stats_->setText("FPS -- | A/V -- ms | Drop -- | A -- / V -- | Buf -- ms");
    }
}

// GUI 持有数据库连接。每个成功打开的本地请求最多做一次查询或入库，再核对编号后记录历史。
void MainWindow::syncLocalLibrary(const PlaybackSnapshot& snapshot)
{
    if (snapshot.source_kind != PlaybackSourceKind::LocalFile || library_request_id_ == 0 ||
        snapshot.request_id != library_request_id_) return;
    if (snapshot.state == PlaybackState::Failed || snapshot.state == PlaybackState::Idle ||
        snapshot.state == PlaybackState::Stopping) {
        library_request_id_ = 0;
        return;
    }
    if (snapshot.state == PlaybackState::Opening || snapshot.media_info.file_path.empty()) return;

    library_request_id_ = 0;
    if (!db_connected_) return;
    auto info = snapshot.media_info;
    MediaInfo db_info;
    if (database_->getMediaByPath(info.file_path, db_info)) {
        info.db_id = db_info.db_id;
        info.is_favorite = db_info.is_favorite;
        info.title = db_info.title;
        info.tags = db_info.tags;
    } else {
        info.db_id = database_->addMedia(info);
    }
    if (info.db_id > 0 && player_->updateLocalLibraryInfo(snapshot.request_id, info)) {
        database_->recordPlayHistory(info.db_id);
    }
}

void MainWindow::toggleMute()
{
    if (isClosing()) return;
    if (is_muted_) {
        // 解除静音，恢复之前的音量
        slider_volume_->setValue(last_volume_);
        is_muted_ = false;
        btn_mute_->setChecked(false);
        btn_mute_->setText("🔊");
        statusBar()->showMessage("解除静音", 2000);
    } else {
        // 静音，保存当前音量
        last_volume_ = slider_volume_->value();
        slider_volume_->setValue(0);
        is_muted_ = true;
        btn_mute_->setChecked(true);
        btn_mute_->setText("🔇");
        statusBar()->showMessage("静音", 2000);
    }
}

// ============================================================
// 辅助方法
// ============================================================
// 本地和直播共用列表，但控件永远只保存脱敏标签，不持有原地址。
int MainWindow::appendPlaylistEntry(PlaybackSource source)
{
    if (isClosing() || !source.isValid()) return -1;
    QString label;
    if (source.kind() == PlaybackSourceKind::NetworkLive && source.protocol() == PlaybackProtocol::Rtsp) {
        label = rtspLabel(source.liveOptions()->rtsp_transport) +
                QString(" #%1").arg(next_network_playlist_number_++);
    } else {
        label = QString::fromStdString(source.displayName());
    }
    playlist_.push_back(std::move(source));
    list_playlist_->addItem(label);
    return static_cast<int>(playlist_.size()) - 1;
}

// 列表双击、上一项/下一项和初始输入保留每条源各自的传输设置。
void MainWindow::openPlaylistEntry(int row)
{
    if (isClosing() || row < 0 || row >= static_cast<int>(playlist_.size())) return;
    if (requestPlayback(playlist_[row]) == 0) return;
    current_playlist_index_ = row;
    list_playlist_->setCurrentRow(row);
}

// 关闭开始后拒绝新的源请求，只有本地文件登记待处理的媒体库请求。
std::uint64_t MainWindow::requestPlayback(const PlaybackSource& source)
{
    if (isClosing() || !source.isValid()) return 0;
    const auto request = player_->requestOpen(source);
    if (request == 0) return 0;
    playback_request_id_ = request;
    library_request_id_ = source.kind() == PlaybackSourceKind::LocalFile ? request : 0;
    // 直接请求与列表打开共用入口，列表调用方在成功后再绑定活动行。
    current_playlist_index_ = -1;
    // 立即刷新请求对应的控件，媒体库同步仍由定时器处理。
    const auto snapshot = player_->getPlaybackSnapshot();
    updatePlaybackControls(snapshot);
    updateMediaInfoPanel(snapshot);
    return request;
}

void MainWindow::setInitialFiles(const std::vector<std::string>& files)
{
    if (isClosing() || files.empty()) return;
    int first_row = -1;
    bool rejected = false;
    for (const auto& file : files) {
        const int row = appendPlaylistEntry(PlaybackSource::localFile(file));
        if (row < 0) rejected = true;
        else if (first_row < 0) first_row = row;
    }
    if (first_row >= 0) openPlaylistEntry(first_row);
    else if (rejected) statusBar()->showMessage("启动参数只接受本地文件；RTSP 请使用“打开网络媒体”。");
}

bool MainWindow::connectDatabase(const std::string& host, const std::string& user,
                                  const std::string& password, const std::string& database)
{
    if (isClosing()) return false;
    if (database_->connect(host, user, password, database)) {
        return database_->initTables();
    }
    return false;
}

// ============================================================
// 关闭与键盘事件
// ============================================================
void MainWindow::closeEvent(QCloseEvent* event)
{
    if (close_state_ == CloseState::Ready) {
        QMainWindow::closeEvent(event);
        return;
    }
    event->ignore();
    if (isClosing()) return;

    // 先关闭命令入口，再同时发出取消。事件循环继续处理队列与窗口关闭。
    close_state_ = CloseState::WaitingForWorkers;
    info_timer_.stop();
    library_request_id_ = 0;
    setAcceptDrops(false);
    for (auto* shortcut : findChildren<QShortcut*>()) shortcut->setEnabled(false);
    for (auto* action : findChildren<QAction*>()) action->setEnabled(false);
    video_widget_->setUpdatesEnabled(false);
    setEnabled(false);
    statusBar()->showMessage("正在关闭，等待任务结束...");

    player_->requestShutdown();
    if (transcoder_) transcoder_->cancel();
    rejectOwnedDialog();
    close_timer_.start();
}

// 原生文件对话框可能不派发 Qt 定时器，首次关闭事件也要直接拒绝它。
void MainWindow::rejectOwnedDialog()
{
    if (modal_depth_ == 0) return;
    auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (active && active->parentWidget() == this) {
        active->reject();
        return;
    }

    // Windows 原生文件对话框可能没有出现在 Qt 的模态窗口列表中。
    const auto dialogs = findChildren<QFileDialog*>(Qt::FindDirectChildrenOnly);
    if (!dialogs.isEmpty()) {
        QPointer<QFileDialog> dialog = dialogs.back();
        dialog->reject();
        if (dialog) dialog->hide();
    }
}

// 等线程发布退出状态与嵌套对话框返回，再接受第二次关闭事件。
void MainWindow::finishCloseWhenReady()
{
    if (close_state_ != CloseState::WaitingForWorkers) return;
    if (modal_depth_ != 0) {
        rejectOwnedDialog();
        return;
    }
    if (!player_->isShutdownComplete() || transcoding_.load()) return;

    close_timer_.stop();
    close_state_ = CloseState::Ready;
    close();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (isClosing()) {
        event->ignore();
        return;
    }
    // 强制捕获 Esc 键退出程序，无论焦点在哪
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

// 桌面拖放只处理本地文件 URL，不把网络地址自动转换成直播请求。
void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (isClosing()) return;
    const auto urls = event->mimeData()->urls();
    if (std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); })) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    if (isClosing()) {
        event->ignore();
        return;
    }
    const QList<QUrl> urlList = event->mimeData()->urls();
    int last_row = -1;
    for (const QUrl& url : urlList) {
        if (!url.isLocalFile()) continue;
        const int row = appendPlaylistEntry(PlaybackSource::localFile(url.toLocalFile().toStdString()));
        if (row >= 0) last_row = row;
    }

    if (last_row < 0) return;
    event->acceptProposedAction();
    const auto snapshot = player_->getPlaybackSnapshot();
    if (snapshot.request_id == playback_request_id_ && canOpenSelectedSource(snapshot.state)) {
        openPlaylistEntry(last_row);
    }
}
