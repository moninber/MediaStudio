# MediaStudio 学习使用文档

## 目录

1. [项目整体框架](#1-项目整体框架)
2. [阅读顺序与学习路径](#2-阅读顺序与学习路径)
3. [核心模块深度解析](#3-核心模块深度解析)
4. [FFmpeg音视频处理](#4-ffmpeg音视频处理)
5. [OpenGL视频渲染](#5-opengl视频渲染)
6. [音频处理与可视化](#6-音频处理与可视化)
7. [Qt GUI与多线程](#7-qt-gui与多线程)
8. [MySQL数据库集成](#8-mysql数据库集成)
9. [媒体转码](#9-媒体转码)
10. [问题排查与调试思路](#10-问题排查与调试思路)
11. [核心概念速查表](#11-核心概念速查表)

---

## 1. 项目整体框架

### 1.1 技术栈全景图

```
┌─────────────────────────────────────────────────────────────────┐
│                         应用层 (Application Layer)                 │
│  MainWindow (Qt QMainWindow) - 用户界面、事件处理、播放列表管理      │
├─────────────────────────────────────────────────────────────────┤
│                      控制层 (Controller Layer)                     │
│  PlayerController - 播放状态管理、线程同步、播放列表控制              │
├─────────────────────────────────────────────────────────────────┤
│                      解码层 (Decoder Layer)                        │
│  MediaDecoder (FFmpeg) - 解封装、解码、色彩转换、音频重采样          │
├─────────────────────────────────────────────────────────────────┤
│                      输出层 (Output Layer)                         │
│  VideoWidget (Qt OpenGL) - 视频帧显示、OpenGL 渲染                  │
│  VideoRenderer (OpenGL) - 着色器特效、纹理管理                       │
│  AudioOutput (WASAPI) - 音频播放、音量控制                          │
│  AudioVisualizer (FFT) - 音频频谱可视化                             │
├─────────────────────────────────────────────────────────────────┤
│                      数据层 (Data Layer)                           │
│  MediaDatabase (MySQL) - 媒体库、收藏、播放历史                       │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 数据流向图

```
用户操作 (点击/键盘/拖拽)
    ↓
MainWindow (Qt 事件)
    ↓
PlayerController (状态管理)
    ↓
    ├─→ MediaDecoder (FFmpeg 解码)
    │       ├─→ av_read_frame() (读取压缩包)
    │       ├─→ avcodec_send/receive() (解码)
    │       ├─→ sws_scale() (YUV→RGB)
    │       └─→ swr_convert() + atempo (音频重采样+变速)
    │
    ├─→ VideoWidget (Qt OpenGL 显示)
    │       └─→ VideoRenderer (OpenGL 渲染)
    │               └─→ glTexSubImage2D() (纹理上传)
    │
    ├─→ AudioOutput (WASAPI 播放)
    │       └─→ IAudioRenderClient (PCM 写入)
    │
    └─→ AudioVisualizer (FFT 可视化)
            └─→ fft() + 汉宁窗 (频谱分析)
```

### 1.3 线程模型

#### 当前架构（第三步多线程重构）

```
┌─────────────────────────────────────────────────────────────┐
│                    Qt 主线程 (GUI Thread)                       │
│  - QApplication 事件循环                                       │
│  - 用户输入处理 (鼠标/键盘/菜单)                               │
│  - VideoWidget::paintGL() (OpenGL 渲染)                        │
│  - UI 控件更新 (进度条/按钮/状态栏)                             │
│  - QMetaObject::invokeMethod (跨线程 UI 更新)                   │
├─────────────────────────────────────────────────────────────┤
│            demux/control 线程 (主线程)                          │
│  - PlayerController::playbackThread() 主循环                   │
│  - decoder_->readPacket() (读取 AVPacket)                      │
│  - 判断音频/视频流，分发到对应队列                               │
│  - seek 处理：清空队列 + decoder_->seek()                      │
│  - 变速处理：等待旧缓冲排空后再切速，再更新 setPlaybackSpeed() │
│  - 队列流控：队列超过 80 个包时休眠                              │
├─────────────────────────────────────────────────────────────┤
│                  音频工作线程 (Audio Worker)                     │
│  - audio_packets_.pop() (从队列取包)                           │
│  - decoder_->decodeAudioPacket() (解码音频)                    │
│  - 音量调整 → audio_output_->play() (WASAPI 播放)               │
│  - 更新音频主时钟 (audio_clock_base + playedSeconds * speed)    │
│  - FFT 频谱可视化 → video_widget_->setSpectrumData()           │
│  - generation 检查：跳过旧车次的帧                              │
├─────────────────────────────────────────────────────────────┤
│                  视频工作线程 (Video Worker)                     │
│  - video_packets_.pop() (从队列取包)                           │
│  - decoder_->decodeVideoPacket() (解码视频)                    │
│  - 音视频同步：根据音频主时钟等待/丢帧                          │
│  - 纯视频文件：根据单调时钟同步                                 │
│  - video_widget_->setVideoFrame() (渲染视频帧)                  │
│  - generation 检查：跳过旧车次的帧                              │
├─────────────────────────────────────────────────────────────┤
│              异步加载线程 (Async Load Thread)                    │
│  - MainWindow::requestLoadAndPlay() 中的 std::thread            │
│  - player_->loadFile() (文件加载)                              │
│  - 数据库同步 (getMediaByPath / addMedia / recordPlayHistory)   │
└─────────────────────────────────────────────────────────────┘
```

#### 关键设计：generation 机制（车次号）

- **类比**：generation = 车次号，packet/frame = 乘客，音频/视频线程 = 接站的人
- **seek/变速时**：`stream_generation.fetch_add(1)` 换车次
- **旧包处理**：队列里的旧车次包被丢弃（`packet_generation != stream_generation`）
- **避免混用**：旧速度的包不会用新速度解码，避免音频忽快忽慢

#### 关键设计：音频主时钟

- **WASAPI 播放进度**：`audio_output_->getPlayedSeconds()` 返回已播放秒数
- **时钟计算**：`audio_clock_base + playedSeconds * active_speed`
- **视频同步**：视频帧根据音频主时钟等待/丢帧
- **纯视频文件**：无音频时使用单调时钟同步（`playback_anchor` + `anchor_pts`）；纯音频仍使用音频输出时钟

#### 关键设计：播放调试信息

播放器状态栏会显示一组轻量统计：渲染 FPS、解码 FPS、A/V diff、丢帧总数、音频/视频队列和音频缓冲。它们主要用于快速判断是解码、同步还是输出侧出现压力。

当前“渲染 FPS”实际统计视频 worker 向控件提交帧的次数；A/V diff 在进入同步等待前记录，停滞时二者都可能保留旧值。判断播放或 EOF 是否完成，需要同时观察媒体位置、输出与线程收尾，不能只看统计数值或“正在播放”的文字。声卡环回能够提供端点输出的证据，但包含系统混音，不能替代主观听感、精确音画同步或尾音完整性验证。

#### 与单线程版本对比

| 特性 | 单线程版本 | 多线程版本（第三步） |
|------|----------|-------------------|
| 线程数 | 1 个播放线程 | 3 个线程（demux + 音频 + 视频） |
| 解码方式 | 顺序解码 | 并行解码 |
| 音视频同步 | 视频时钟优先 | 音频主时钟 |
| 队列 | 无 | PacketQueue（线程安全） |
| seek/变速 | 直接调用 | generation 机制 + 队列清空 |
| 性能 | 单核受限 | 多核并行 |

### 1.4 核心依赖库


| 库名                  | 版本          | 用途          | 关键 API                                              |
| ------------------- | ----------- | ----------- | --------------------------------------------------- |
| Qt                  | 6.11        | GUI 框架      | QApplication, QMainWindow, QOpenGLWidget, QShortcut |
| FFmpeg              | 8.0         | 音视频编解码      | avformat, avcodec, swscale, swresample, avfilter    |
| OpenGL              | 3.3 Core    | 图形渲染        | glGenTextures, glTexSubImage2D, glUseProgram        |
| GLEW                | 2.3.1       | OpenGL 扩展加载 | glewInit, glewGetProcAddress                        |
| WASAPI              | Windows API | 音频输出        | IAudioClient, IAudioRenderClient                    |
| MySQL Connector/C++ | 8.0         | 数据库连接       | sql::Connection, sql::PreparedStatement             |


---

## 2. 阅读顺序与学习路径

### 2.1 建议阅读顺序（由浅入深）

阅读代码时，**顺着数据流走**最容易理解：


| 阶段        | 文件                        | 作用            | 需要关注的重点                                                                                              | 技术栈                        |
| --------- | ------------------------- | ------------- | ---------------------------------------------------------------------------------------------------- | -------------------------- |
| **入口**    | `main.cpp`                | 程序入口          | COM 初始化、QApplication 创建、命令行参数处理                                                                      | Qt, COM                    |
| **界面声明**  | `MainWindow.h`            | GUI 声明        | UI 控件、信号槽、核心成员变量、快捷键方法                                                                               | Qt Widgets                 |
| **界面实现**  | `MainWindow.cpp`          | GUI 实现 + 事件处理 | `requestLoadAndPlay()` 异步加载、`setupShortcuts()` 全局快捷键、数据库同步逻辑                                         | Qt, 多线程, MySQL             |
| **控制枢纽**  | `PlayerController.h/.cpp` | 播放控制          | `loadFile()`/`play()`/`stop()` 生命周期、`playbackThread()` 三线程架构、`stream_generation` 机制、音频主时钟 | C++17, std::thread, atomic |
| **队列**     | `PacketQueue.h/.cpp`      | 线程安全队列       | `push()`/`pop()` 阻塞等待、`abort()`/`finish()` 终止标志、线程同步                                          | C++17, std::mutex, condition_variable |
| **解码器**   | `MediaDecoder.h/.cpp`     | FFmpeg 解码     | `readPacket()`/`decodeAudioPacket()`/`decodeVideoPacket()` 分离接口、`setPlaybackSpeed()` atempo 滤镜重建       | FFmpeg, 音视频处理              |
| **音频输出**  | `AudioOutput.h/.cpp`      | WASAPI 音频     | `init()` 六步流程、`play()` 缓冲区写入、`getPlayedSeconds()` 音频主时钟、`reset()` 重置                               | WASAPI, COM                |
| **视频显示**  | `VideoWidget.h/.cpp`      | Qt OpenGL     | `QOpenGLWidget` 三件套、帧缓冲互斥锁、`clearFrame()`、焦点策略 `NoFocus`                                             | Qt OpenGL, 多线程             |
| **视频渲染**  | `VideoRenderer.h/.cpp`    | OpenGL 渲染     | 着色器编译、纹理上传、`glTexSubImage2D`、7 种 GLSL 特效                                                             | OpenGL, GLSL               |
| **音频可视化** | `AudioVisualizer.h/.cpp`  | FFT 频谱        | `fft()` Cooley-Tukey 实现、窗函数、分贝归一化                                                                    | 数字信号处理                     |
| **数据结构**  | `MediaInfo.h`             | 结构定义          | `MediaInfo`、`VideoStreamInfo`、`AudioStreamInfo`、`VideoEffect` 枚举、`db_id`、`is_favorite`               | C++ 结构体                    |
| **数据库**   | `MediaDatabase.h/.cpp`    | MySQL 媒体库     | MariaDB C API、`mysql_real_connect()`、`mysql_query()`、CRUD 操作、播放历史记录                                    | MariaDB/MySQL C API         |
| **转码器**   | `MediaTranscoder.h/.cpp`  | 格式转码          | FFmpeg 编码 API 完整链路、PTS 换算、YUV420P 兼容性                                                                | FFmpeg 编码                  |


### 2.2 架构理解口诀

- **一条主线**：`MainWindow` 收到用户操作 → 调用 `PlayerController` → `MediaDecoder` 解码 → `AudioOutput` 播放音频 + `VideoWidget` 显示视频
- **三个线程**：Qt 主线程（GUI + OpenGL）+ demux/control 线程（读包分发）+ 音频线程（解码播放）+ 视频线程（解码渲染）
- **两个队列**：`audio_packets_`（音频包队列）+ `video_packets_`（视频包队列）
- **generation 机制**：seek/变速时换车次，旧车次的包被丢弃
- **音频主时钟**：WASAPI 播放进度作为时间基准，视频根据音频时钟同步
- **四道关卡**：`load_request_id_`（UI 层去重）→ `lifecycle_mutex_`（控制器层互斥）→ `session_id_`（播放线程层会话隔离）→ `stream_generation`（队列层车次隔离）
- **五个同步**：原子变量（标志位）、互斥锁（临界区）、递归锁（生命周期）、条件变量（队列阻塞）、Qt 队列调用（跨线程 UI 更新）

### 2.3 分步改进与文档维护

网络能力改造按“阶段.编号”逐项推进，默认每轮完成一个子改动。每轮在聊天中说明编号、改动、验证结果、未通过项、阅读顺序和下一项；相关项目改动验证后创建 Git 本地提交，经用户确认该编号完成后再推送一次。

长期内容按用途更新现有文档：操作方法放在 `USAGE.md`，必要的技术解释放在 `LEARNING.md`，完成的改动摘要放在 `SUMMARY.md`。不为每次实验或改动另建进度、报告或交接文档，单次验证细节直接在聊天中说明，原始日志和素材留在项目外的验证目录。

---

## 3. 核心模块深度解析

### 3.1 MainWindow - 用户界面与事件中心

#### 技术栈

- Qt 6.11 (QMainWindow, QDockWidget, QShortcut, QMetaObject)
- 多线程 (std::thread, std::atomic, std::mutex)
- MySQL Connector/C++ (sql::Connection, sql::PreparedStatement)

#### 核心逻辑

**1. 异步文件加载 (`requestLoadAndPlay`)**

```cpp
// 问题：快速切换文件时，多个 loadFile() 并发调用导致 FFmpeg 状态损坏
// 解决：使用原子 ID + 互斥锁实现请求序列化
void MainWindow::requestLoadAndPlay(const std::string& path, const QString& display_name) {
    // 生成唯一请求 ID
    int request_id = ++load_request_id_;
    
    std::thread([this, path, display_name, request_id]() {
        std::lock_guard<std::mutex> lock(load_mutex_);
        // 检查请求是否过期（如果有新请求到达，旧请求直接返回）
        if (request_id != load_request_id_.load()) return;
        
        // 加载文件
        bool loaded = player_->loadFile(path);
        
        // 数据库同步逻辑
        if (loaded && db_connected_) {
            MediaInfo info = player_->getMediaInfo();
            MediaInfo db_info;
            if (database_->getMediaByPath(path, db_info)) {
                // 已存在：同步数据库中的 ID 和收藏状态
                info.db_id = db_info.db_id;
                info.is_favorite = db_info.is_favorite;
            } else {
                // 不存在：添加到数据库
                int id = database_->addMedia(info);
                info.db_id = id;
            }
            player_->setMediaInfo(info);
            database_->recordPlayHistory(info.db_id);
        }
        
        // 跨线程更新 UI
        QMetaObject::invokeMethod(this, [this]() {
            btn_play_->setText("⏸");
            statusBar()->showMessage("正在播放: xxx");
        }, Qt::QueuedConnection);
    }).detach();
}
```

**2. 全局快捷键 (`setupShortcuts`)**

```cpp
// 问题：UI 控件焦点导致快捷键失效（如 VideoWidget 抢占焦点）
// 解决：使用 QShortcut + ApplicationShortcut 上下文
void MainWindow::setupShortcuts() {
    // Space - 播放/暂停
    QShortcut* shortcutSpace = new QShortcut(QKeySequence(Qt::Key_Space), this);
    shortcutSpace->setContext(Qt::ApplicationShortcut);  // 全局生效
    connect(shortcutSpace, &QShortcut::activated, this, &MainWindow::togglePlayPause);
    
    // Esc - 退出
    QShortcut* shortcutEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    shortcutEsc->setContext(Qt::ApplicationShortcut);
    connect(shortcutEsc, &QShortcut::activated, this, &MainWindow::close);
    
    // F - 收藏切换
    QShortcut* shortcutF = new QShortcut(QKeySequence(Qt::Key_F), this);
    shortcutF->setContext(Qt::ApplicationShortcut);
    connect(shortcutF, &QShortcut::activated, this, [this]() {
        if (db_connected_) {
            MediaInfo info = player_->getMediaInfo();
            info.is_favorite = !info.is_favorite;
            database_->updateMedia(info);
            player_->setMediaInfo(info);
        }
    });
}
```

**3. 焦点策略修复**

```cpp
// VideoWidget.cpp
VideoWidget::VideoWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::NoFocus);  // 关键：不抢占焦点，确保 Esc 等全局快捷键生效
}
```

#### 使用方法

- **打开文件**：菜单 文件 → 打开，或拖拽文件到窗口
- **播放控制**：工具栏按钮或快捷键（Space 播放/暂停、Esc 退出、↑/↓ 音量）
- **数据库连接**：菜单 工具 → 连接数据库，输入 MySQL 连接信息
- **收藏功能**：按 F 键切换收藏状态（需先连接数据库）

### 3.2 PlayerController - 播放状态与线程管理

#### 技术栈

- C++17 (std::thread, std::atomic, std::recursive_mutex, std::condition_variable)
- FFmpeg (通过 MediaDecoder)
- WASAPI (通过 AudioOutput)

#### 核心逻辑

**1. 生命周期互斥 (`lifecycle_mutex`_)**

```cpp
// 问题：loadFile() 内部调用 stop()，两者都需要加锁，std::mutex 会死锁
// 解决：使用 std::recursive_mutex 允许同一线程重复加锁
bool PlayerController::loadFile(const std::string& filename) {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    stop();  // stop() 内部也要加同一把锁，递归锁允许
    // ... 打开新文件
}

void PlayerController::stop() {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    // ... 停止逻辑
}
```

**2. 会话隔离 (`session_id_`)**

```cpp
// 问题：旧播放线程在新文件加载后继续运行，导致状态混乱
// 解决：每次 play() 递增 session_id_，旧线程检测到不匹配则退出
void PlayerController::play() {
    session_id_++;  // 新会话
    playing_ = true;
    paused_ = false;
    if (!playback_thread_.joinable()) {
        playback_thread_ = std::thread(&PlayerController::playbackThread, this);
    }
}

void PlayerController::playbackThread() {
    const int my_session = session_id_.load();  // 记录当前会话 ID
    while (!stop_requested_ && session_id_.load() == my_session) {
        // 播放逻辑
    }
    // session_id_ 变化时自动退出
}
```

**3. 视频时钟优先 (倍速时间跳动修复)**

```cpp
// 问题：倍速时音频/视频 PTS 混合更新导致时间显示跳动
// 解决：优先使用视频时钟，无视频时才用音频时钟
const bool use_video_clock = decoder_->hasVideo();

if (use_video_clock && frame.type == DecodedFrame::Type::Video) {
    // 视频帧：直接使用视频 PTS
    current_time_ = frame.pts;
} else if (!use_video_clock && frame.type == DecodedFrame::Type::Audio) {
    // 纯音频：使用音频 PTS
    current_time_ = frame.pts;
}
// 避免：音频/视频 PTS 混合更新导致时间跳动
```

#### 使用方法

- **加载文件**：`loadFile(path)` - 打开媒体文件
- **播放控制**：`play()` / `pause()` / `stop()` / `seek(seconds)`
- **参数调节**：`setVolume(0.0-1.0)` / `setSpeed(0.25-4.0)` / `setEffect(effect)`
- **播放列表**：`setPlaylist(files)` / `nextFile()` / `prevFile()`

### 3.3 MediaDecoder - FFmpeg 解码核心

#### 技术栈

- FFmpeg 8.0 (avformat, avcodec, swscale, swresample, avfilter)
- C++17 (std::unique_ptr, RAII)

#### 核心逻辑

**1. 解码流程**

```cpp
bool MediaDecoder::open(const std::string& filename) {
    // 1. 打开文件
    avformat_open_input(&format_ctx_, filename, nullptr, nullptr);
    // 2. 探测流信息
    avformat_find_stream_info(format_ctx_, nullptr);
    // 3. 查找解码器
    AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    // 4. 创建解码器上下文
    video_codec_ctx_ = avcodec_alloc_context3(codec);
    // 5. 复制参数
    avcodec_parameters_to_context(video_codec_ctx_, codec_params);
    // 6. 打开解码器
    avcodec_open2(video_codec_ctx_, codec, nullptr);
    // 7. 初始化转换上下文
    sws_ctx_ = sws_getContext(...);  // 视频色彩转换
    swr_ctx_ = swr_alloc_set_opts(...);  // 音频重采样
}

bool MediaDecoder::decodeVideoPacket(const AVPacket* packet, std::vector<DecodedFrame>& out_frames) {
    // 1. 将压缩包发送给解码器
    avcodec_send_packet(video_codec_ctx_, packet);
    // 2. 拉取当前可用的视频帧
    receiveVideoFrames(out_frames);
    // 4. 转换格式
    if (video) sws_scale(sws_ctx_, ...);  // YUV→RGB
    if (audio) swr_convert(swr_ctx_, ...);  // 重采样 + atempo
}
```

**2. 音频倍速滤波器 (`atempo`)**

```cpp
// 问题：倍速时音频变调或静音
// 解决：使用 FFmpeg atempo 滤波器
bool MediaDecoder::initAudioFilterGraph() {
    // 创建滤波图：abuffer → atempo → abuffersink
    audio_filter_graph_ = avfilter_graph_alloc();
    
    // abuffer (音频源)
    avfilter_graph_create_filter(&audio_buffer_src_ctx_, 
        avfilter_get_by_name("abuffer"), "in", args, nullptr, audio_filter_graph_);
    
    // atempo (变速)
    std::string filter_desc = buildAtempoFilterChain(playback_speed_);
    avfilter_graph_parse_ptr(audio_filter_graph_, filter_desc.c_str(), 
        &inputs, &outputs, nullptr);
    
    // abuffersink (音频接收)
    avfilter_graph_create_filter(&audio_buffer_sink_ctx_,
        avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, audio_filter_graph_);
    
    avfilter_graph_config(audio_filter_graph_, nullptr);
}

std::string MediaDecoder::buildAtempoFilterChain(float speed) const {
    if (speed == 1.0f) return "";  // 1.0x 时 bypass
    // atempo 只支持 0.5-2.0x，需要级联实现更大倍速
    if (speed > 2.0f) {
        return "atempo=2.0,atempo=" + std::to_string(speed / 2.0f);
    } else if (speed < 0.5f) {
        return "atempo=0.5,atempo=" + std::to_string(speed / 0.5f);
    } else {
        return "atempo=" + std::to_string(speed);
    }
}
```

#### 使用方法

- **打开文件**：`open(path)` - 初始化解码器
- **解码视频包**：`decodeVideoPacket(packet, frames)` - 从视频包获取解码帧
- **解码音频包**：`decodeAudioPacket(packet, frames)` - 从音频包获取解码帧
- **跳转**：`seek(seconds)` - 跳转到指定时间
- **倍速**：`setPlaybackSpeed(speed)` - 设置播放速度
- **获取信息**：`getMediaInfo()` - 获取媒体元数据

### 3.4 MediaDatabase - MySQL 媒体库

#### 技术栈

- MySQL Connector/C++ 8.0
- SQL (PreparedStatement, 事务)

#### 核心逻辑

**1. 连接管理 (Debug/Release 兼容)**

```cpp
// 问题：Debug 模式下连接数据库崩溃（STL 二进制不兼容）
// 解决：使用 ConnectOptionsMap + 建议 Release 模式
bool MediaDatabase::connect(const std::string& host, const std::string& user,
                             const std::string& password, const std::string& database) {
    try {
        driver_ = sql::mysql::get_mysql_driver_instance();
        sql::ConnectOptionsMap connection_options;
        connection_options["hostName"] = host;
        connection_options["userName"] = user;
        connection_options["password"] = password;
        connection_options["schema"] = database;
        connection_options["port"] = 3306;
        connection_options["OPT_RECONNECT"] = true;
        connection_ = driver_->connect(connection_options);
        return connection_ != nullptr;
    } catch (const sql::SQLException& e) {
        last_error_ = std::string("连接失败: ") + e.what();
        return false;
    }
}
```

**2. 路径查询 (`getMediaByPath`)**

```cpp
// 用途：加载文件时检查是否已在数据库中
bool MediaDatabase::getMediaByPath(const std::string& path, MediaInfo& info) {
    sql::PreparedStatement* pstmt = connection_->prepareStatement(
        "SELECT * FROM media_library WHERE file_path = ?"
    );
    pstmt->setString(1, path);
    sql::ResultSet* rs = pstmt->executeQuery();
    bool found = false;
    if (rs->next()) {
        readMediaFromResultSet(rs, info);
        found = true;
    }
    delete rs;
    delete pstmt;
    return found;
}
```

**3. 自动同步逻辑**

```cpp
// MainWindow.cpp 中的调用
if (db_connected_) {
    MediaInfo info = player_->getMediaInfo();
    MediaInfo db_info;
    if (database_->getMediaByPath(path, db_info)) {
        // 已存在：同步数据库状态
        info.db_id = db_info.db_id;
        info.is_favorite = db_info.is_favorite;
        info.title = db_info.title;
        info.tags = db_info.tags;
    } else {
        // 不存在：插入新记录
        int id = database_->addMedia(info);
        info.db_id = id;
    }
    player_->setMediaInfo(info);
    database_->recordPlayHistory(info.db_id);
}
```

#### 使用方法

- **连接数据库**：`connect(host, user, password, database)`
- **添加媒体**：`addMedia(info)` - 插入新记录
- **查询媒体**：`getMediaById(id)` / `getMediaByPath(path)`
- **更新媒体**：`updateMedia(info)` - 更新收藏状态等
- **播放历史**：`recordPlayHistory(media_id)` - 记录播放
- **获取收藏**：`getFavorites()` - 获取收藏列表

---

## 4. FFmpeg音视频处理

### 4.1 核心概念

FFmpeg处理媒体文件的核心数据流：

```
媒体文件 → AVPacket(压缩包) → AVFrame(原始帧) → 像素/音频数据
         解封装(av_read_frame)    解码(avcodec_send/receive)
```

**四个核心结构体**：


| 结构体               | 含义      | 类比    |
| ----------------- | ------- | ----- |
| `AVFormatContext` | 文件格式上下文 | 书的目录  |
| `AVCodecContext`  | 编解码器上下文 | 翻译官   |
| `AVPacket`        | 压缩数据包   | 压缩的包裹 |
| `AVFrame`         | 解码后的帧   | 打开的包裹 |


### 4.2 代码学习路线

**第一步**：阅读 `MediaDecoder.cpp` 的 `open()` 函数

以下是简化的调用顺序。实际打开前先分配格式上下文并安装中断回调，再按源类型设置输入选项。

```cpp
// 打开、探测、初始化解码器：
avformat_open_input(&format_ctx_, filename, nullptr, nullptr);  // 1.打开文件
avformat_find_stream_info(format_ctx_, nullptr);                  // 2.探测流信息
avcodec_find_decoder(codec_params->codec_id);                     // 3.找解码器
avcodec_alloc_context3(codec);                                     // 4.创建上下文
avcodec_parameters_to_context(codec_ctx_, codec_params);           // 5.复制参数
avcodec_open2(codec_ctx_, codec, nullptr);                         // 6.打开解码器
```

**第二步**：阅读 `decodeVideoFrame()` 函数

```cpp
// 解码循环的核心：
while (av_read_frame(format_ctx_, packet) >= 0) {           // 读取压缩包
    if (packet->stream_index == video_stream_index_) {       // 过滤视频流
        avcodec_send_packet(codec_ctx_, packet);              // 发送给解码器
        avcodec_receive_frame(codec_ctx_, frame);             // 获取解码帧
    }
}
```

**第三步**：理解色彩空间转换

```cpp
// 视频解码后是YUV格式，OpenGL需要RGB
sws_scale(sws_ctx_,           // 转换上下文
    frame->data,              // 源数据（YUV）
    frame->linesize,          // 源行跨度
    0, frame->height,         // 起始行和行数
    rgb_data, rgb_linesize);  // 目标数据（RGB）
```

### 4.3 关键知识点

- **时间基(Time Base)**：FFmpeg用分数表示时间单位，`av_q2d()` 转为秒
- **seek操作**：必须seek到关键帧（`AVSEEK_FLAG_BACKWARD`），然后继续解码
- **刷新缓冲区**：seek后必须 `avcodec_flush_buffers()` 清除旧帧
- **文件结束**：发送 `nullptr` packet可刷新解码器中的剩余帧

### 4.4 输入类型与可取消 I/O

1. 从 `PlaybackSource.h` 和 `PlaybackSource.cpp` 看三类输入的策略。直播和点播共用 I/O 基础设施，但分别描述暂停、EOF、队列满和恢复落点。点播初始能力不会从 URL 或已知时长推断出可 seek、可倍速或可原位恢复。
2. 从 `MediaIo.h` 和 `MediaDecoder::open()` 看一次打开尝试怎样拥有取消对象。打开、探测和每次读取分别设置单调时钟期限；FFmpeg 回调只读取原子状态和时钟。取消后不复位该对象，收尾关闭格式上下文后再释放它。本地文件不套网络读取期限。
3. 从 `readPacketResult()` 看读包结果：包、暂时无包、EOF、超时、主动取消和其他错误各自保留 FFmpeg 返回码。结果分类负责描述这一次读取，正常 EOF 排空和直播恢复属于控制器策略。
4. 从 `PlaybackSnapshot.h` 看 UI 如何按值读取状态、能力和媒体元信息。媒体库只回写匹配请求编号的本地库字段；读取快照不持有解码或生命周期锁。

### 4.5 播放命令与输出标签

主窗口把打开和停止提交给 `PlayerController`，定时读取快照。打开命令先发布新的请求编号和取消对象。`supervisorLoop()` 完成旧连接的取消、音视频线程回收和解封装关闭，然后取最新的打开命令。

`PlaybackEpoch` 包含用户请求编号、连接尝试编号和播放代次。解复用线程把当次标签和包一起推入 `PacketQueue`。工作线程使用出队时获得的标签进行解码和提交，seek 或变速刷新后旧包不能继承新代次。`VideoWidget` 在接收和绘制时检查标签。seek 可保留最后绘出的画面，只有当前标签的帧提交后，快照才会标记本代有视频输出。

### 4.6 读包锁与音频设备线程

supervisor 打开媒体后缓存所选音视频流的 time_base。worker 用其把 PTS 的刻度转换为秒。阻塞读包只操作 supervisor 拥有的格式上下文，音视频 worker 可以同时消费已有队列。解码、排空和本地定位引起的编解码器刷新仍受编解码锁保护。

音频线程在自己的作用域内创建和销毁 AudioOutput。本地文件暂停保留未写入 PCM 的帧偏移及设备里已提交的数据，继续从该偏移写入；直播暂停则释放整次连接和设备，见 4.10。seek 提升代次，旧 PCM 失效，音频线程在新一代开始前重置设备。每次采样从同一份 padding 推出已播时长和待播时长，按代次发布给视频同步、倍速刷新和界面统计。

### 4.7 关窗与线程回收

`MainWindow::closeEvent()` 第一次收到关窗请求时忽略事件，封闭入口并发出取消。窗口继续处理 Qt 事件，定时检查播放和转码是否收尾；满足条件才再次关窗。这样 `VideoWidget` 等控件在工作线程最后一次访问期间仍然存活。

`PlayerController::requestShutdown()` 在命令锁下丢弃待打开的请求、取消活动 I/O 并清除输出标签。FFmpeg 的中断回调退出阻塞调用后，supervisor 中止队列唤醒消费者、回收音视频线程，再关闭解码器和输入资源。最后才发布完成信号，主窗口析构时接回线程句柄。普通停止仍允许随后打开新文件，关窗进入的终止状态则不可恢复。

### 4.8 直播入口与混合播放列表

打开网络媒体时，用 `QUrl` 严格检查 RTSP 地址的结构，由 `PlaybackSource` 保存原始编码、源类型和 TCP/UDP 选项。列表、快照和错误提示只使用派生的协议标签；只有打开输入的代码需要原始地址。列表按源对象切换，文件路径和直播 URL 不经过同一个本地文件入口，媒体库同步也只接受本地请求。

直播没有可定位的总时长。界面按快照区分文件进度与直播计时；控制器同时检查源类型，拒绝对直播执行定位或改速。直播按 1.0 倍运行，本地文件的倍速偏好保留，切回文件时恢复。TCP/UDP 的选择只传给 FFmpeg 的 RTSP 输入选项，与界面的控制能力分开。

### 4.9 RTSP 首包、首段声音和首帧

打开和探测完成后，直播先进入 `Buffering`。播放器拿到第一份解复用包时只登记首包；视频包要等到 FFmpeg 标记为关键且未标损坏的包才开始送解码。关键包标志是提示，解码后仍检查帧的损坏标记，跳过的损坏帧与 A/V 同步丢帧分开统计。

有音轨时，第一段成功写入 WASAPI 的 PCM 建立音频媒体时间基准，声卡已播放的采样量驱动后续视频同步。首次对齐时，能够换算的 PTS 也未必与另一轨共享起点：如果首个有效视频 PTS 与此时的音频进度相差超过 1 秒，就给视频建立到音频时钟的偏移，保留视频后续帧之间的间隔。纯视频把第一帧的有效 PTS 对齐到本连接的单调时钟，之后只等时间戳的差值；初始 PTS 再大，也不会变成起播要等待的时间。

音频实际写入声卡、视频被当前会话的控件接受，各自只记一次；需要的轨道都就绪才进入 `Playing`。首输出期限从探测完成后计算，持续收到没有可用画面的包也不能延长期限。首帧绘制由 Qt 的 OpenGL 绘制路径按相同会话标签另记；频谱视图可以继续播放而不绘制视频，留存的静态画面也不算新会话输出。四个里程碑的时间都从当前连接尝试开始算；自动重建会清空这些事件，显式用户请求起点另用于记录连接的相对开始时间。

### 4.10 直播暂停为何需要新请求

本地文件暂停会保留解码器、队列和播放位置；直播暂停采用断开策略。控制器在命令锁内先发布取消并中止队列，撤销原连接和媒体代次的输出归属，将状态改为 `Paused`。已显示的静态帧可以保留，但旧 worker 不能再提交新视频、频谱或进度。Qt 线程只投递命令；音频设备仍由音频 worker 停止和释放，supervisor 等待 worker 退出并关闭解码器。

`is_session_active` 表示 supervisor 还在打开输入或占用、收尾会话资源，不直接等同于网络连接是否存在。因此界面在暂停命令后先显示正在断开，观察到收尾完成后才显示已断开。暂停期间没有待打开命令，直播计时冻结。已成功打开的 RTSP 输入在关闭时给 TEARDOWN 写入单独的 250 ms 期限，取消标志保持不变，也不等待服务端回应；否则 UDP 服务端可能在控制连接断开后仍保留会话。这只用于关闭阶段，不重新允许读包。结束消息是尽力发送，部分握手失败或服务端不响应时仍以本地资源按时释放为准。

继续和“回到直播”复用内部保存的源值及网络选项，创建新的 request ID、I/O 取消对象和连接时钟；新连接重新从关键包开始，首包、PCM、视频提交与绘制重新登记。内部带锁的打开辅助函数避免在已持锁时再次调用公开打开接口。连续操作只保留最新待处理请求；旧连接先退出，才执行新打开。停止和关窗会清除内部源，迟到的继续或回到直播不能使它复活。音量、特效和频谱偏好属于播放器设置，不随连接重置。

### 4.11 失败后的历史与当前输出

进入 `Failed` 时，声音线程和解码资源已经回收，但控件也必须同步撤销旧连接的输出标签。只修改状态文字会留下一个漏洞：未绘制的旧帧还能变成新画面，甚至旧标签的频谱仍被接受。直播失败收尾现在先保存已发生的首帧绘制时间，再撤销待绘制数据和连接/代次，清除当前视频有效标记。已绘制的静态画面可以留下；历史首包、PCM、提交和绘制记录不证明当前还在播放。

切源验收以实际音画输出为边界：先确认上一源有输出，再打开下一源，等待下一源自己的轨道与标签就绪。在不同视频尺寸、声音有无和大 PTS 的源之间切换，能检查旧元信息、PCM 时钟和待绘制数据是否串到新源。向控件提交旧标签应被拒绝；另行核对服务端 TEARDOWN 和声卡释放，避免只凭客户端状态声称旧连接已经退出。

### 4.12 直播有限恢复策略（3.1）

`LiveRecovery` 把故障判定、预算状态机和可取消等待分开。分类器使用源类型、应用状态、操作阶段和原始错误码：主动取消不算网络故障；明确的参数、认证或解码器问题终止重试，暂未发布的 404、临时拒绝的 429、直播 EOF 和读取/输出超时可以有限恢复。本地和点播不进入这套直播规则。应用已判定超时时，FFmpeg 原始返回码即使是 0 也照样保留，不能据此改成成功。

预算从首个可恢复故障开始，最多允许 3 次额外尝试，依次退避 1、2、4 秒。排出候选不计数，只有 `startAttempt()` 真正开始才增加计数；重复故障通知不会重排等待或增加次数。打开、探测和首输出都取“操作期限与剩余 45 秒总预算的较早者”，退避到达总期限也立即终止。

实际音画恢复后进入健康观察，不因打开成功清空预算。需要连续 30 秒所需轨道都有效输出、缓冲处于限界内，并在有可信媒体推进指标时没有持续积压，才能恢复可用次数和下一轮时间预算；本次用户请求的累计自动重试次数仍保留。没有可信时间戳用空值，仍可凭有效输出和受控缓冲恢复。面向每秒健康采样，超过 2 秒的观察空窗重新累计，避免两个相隔很久的快照冒充持续播放；3.2 每秒观测实际输出，并已通过真实 TCP/UDP 重启后的健康输出核验。正常持续出音画不会因旧 45 秒期限到达而被强行停播，但稳定满 30 秒前再次失败仍沿用原预算。

策略只在 supervisor 所在线程驱动，时间由调用方传入，因此测试可以直接推进到 45 秒或 30 秒。退避等待由请求独享的 `RecoveryWait` 使用条件变量等待，取消在短锁下发布并唤醒，覆盖检查与入睡之间的竞争；FFmpeg 的中断回调继续只使用 `MediaIoControl`，不会去等待这个锁。

3.1 先独立实现并验证策略；3.2 已将它接入实际连接流程，见下一节。队列限界事件已在 3.3 接入，见 4.14。自动连接共享本次用户请求的取消对象；显式重新打开、继续和手动回到直播各自创建新的取消对象与恢复预算。

### 4.13 实际自动重连与分轨输出保护（3.2）

先读 `PlayerController::supervisorLoop()`：一个外层循环处理用户请求，内层串行处理初始连接和自动重试。故障发生时启动预算，先停止、接回音视频 worker 并释放声卡，再关闭旧输入；收尾时间也消耗原预算。`retireLiveOutputLocked()` 保存已发生的绘制历史和冻结计时，清除旧连接/媒体代次及待绘制数据，只保留已经显示的静帧。随后 `RecoveryWait` 在不持快照锁时等待退避；每 200 ms 更新倒计时，用户取消通过条件变量立即唤醒。

实际启动下一次连接时，在用户命令使用的同一把锁内重新核对请求仍有效、计数并发布新的 `MediaIoControl`。因此停止不会只取消旧 I/O 而漏掉刚创建的新对象。自动重试保持 request ID，connection attempt ID 递增，generation 从 1 开始；重新打开、继续及回到直播则创建新 request。新连接重新分配输入、解码状态和声卡，清空队列与元信息，重新建立关键包门控、PCM 媒体基准和视频单调时钟锚点，音量/特效/频谱及源选项继续保留。

再读 `MediaIoControl`：单次打开/探测/读取期限、恢复过程的总期限、首次输出期限以及音频/视频各自的输出期限彼此独立，FFmpeg 中断回调只读原子状态。活动轨道从探测完成后各有 10 秒期限；音频仅实际 PCM 写入刷新自己的期限，视频仅有效帧被当前标签接受时刷新自己的期限。两轨都恢复后撤销首次输出和本次打开的总时间限制，持续输出保护仍保留；否则恢复后正常播放到旧 45 秒时仍可能被错误中断。过期轨道不能靠一帧迟到输出把自己复活。

最后读 `demuxAndPlay()` 的 `record_live_output` 和 `update_recovery`：worker 在快照锁内登记真实输出，所需轨道齐备后进入 Playing；supervisor 每秒采样，最近 2 秒内各轨有输出、观察到的包数未超过当前预设阈值、声卡排队不超过 1 秒，并在媒体时钟可换算时推进至少 0.8x，才累计健康时间。时间域跳变撤销该次推进测量；缺失时间戳不能单独阻止预算恢复。连续 30 秒满足条件后清空本轮已用次数，累计实际重试次数保留。3.2 当时仅做包数观测；3.3 已将健康判据换成队列实际包数、字节和可信跨度的限界快照。完整相对积压指标留到阶段 4。

测试先看 `RecoveryTests` 的独立期限，再看 `ControllerTests` 的实际断开重连、1/2/4 秒次数耗尽、退避及第二次自动打开中的暂停/停止/切源/关窗取消、单轨停发和重建后轨道变化。首包、输入就绪、PCM、视频提交和绘制都属于当前连接；频谱隐藏视频时不要求绘制，静帧也不能当作新输出。

### 4.14 队列限界、Full 与可取消回压（3.3）

调用先到 `playbackPolicy()`：它按源类型生成队列策略，稳定直播每轨 512 包、音频 2 MiB/视频 16 MiB、跨度 2 秒；低延迟参数为 256 包、1/8 MiB、750 ms，但完整低延迟 UI 尚未开放。本地限额为 0，保留原有两个活动队列都超过约 80 包时暂停读取的行为；点播策略仍是 Backpressure，输入入口留后续阶段。`supervisorLoop()` 等旧线程退出后清空新连接的队列和统计；`runRequest()` 探测成功后用缓存的音视频 time_base 配置 `PacketQueue::reset(limits, time_base)`，不让 GUI 或队列跨线程访问 AVStream。

随后 `demuxAndPlay()` 读到选中包，音频直接调用内部 `enqueue`，视频先通过已有关键包门控。`enqueue` 根据源策略调用 `PacketQueue::push()`；直播立即返回，本地不套直播限额，未来点播可使用 WaitForSpace。等待模式调用前必须释放播放器快照/命令锁，队列内部的条件变量等待也原子释放队列锁，否则用户取消和消费者出队都可能被阻塞。

`push()` 在同一把队列锁中核对 abort/finish/revision，先判断单包是否过大，再计算“加入这个包之后”会不会超过包数、payload 或可信跨度。没有空间时直播返回带具体限界维度的 Full；单包本身过大返回 PacketTooLarge，等待模式也不会等一个永远装不下的包。只有能够接受时才 `av_packet_clone()` 并插入，随后更新占用/峰值，再在锁外唤醒消费者。Full、过大或取消不克隆原包，分配失败由临时独占指针释放引用，不把拒收包静默跳过。

时间戳由 `timestampCandidateLocked()` 先选择 DTS，缺少 DTS 才回退 PTS，并用缓存 time_base 换算。负 DTS 和很大的绝对 PTS 可以正常使用；减法先转 long double，避免整数极值溢出。缺失、倒退、超过 10 秒的相邻跳变、DTS/PTS 基准切换或 epoch 变化会重新建立测量域；队列中仍有旧域、缺失或跳变包时，`spanLocked()` 返回不可用，不能把不相关的时间戳相减。旧域前缀消费完后，同域可信后缀可重新报告跨度。跨度指排队包首尾时间差，不包含最后一个包的持续时长，也不是端到端延迟。

消费者调用 `pop()` 时，在队列锁内取走条目并扣减包数/字节，释放锁后唤醒等待空间的生产者，再把包引用及原 epoch 移交给消费者。这样出队包不受后来队列清空影响。`finish()` 拒绝新包但允许排空，空消费者和生产者都会被唤醒；`abort()` 则立即释放仍排队的引用并同时唤醒两类等待者，实际解码/声卡资源仍由原 worker 收尾。`reset()` 清空引用、峰值和本连接事件，递增 revision；等待者记住入睡前的 revision，即使 abort 后马上 reset，旧等待也不能把包投进新连接。无参数 reset 保留配置供同连接刷新；新连接和切回文件必须显式更换配置。

拒收结果返回 `enqueue` 后，Full/PacketTooLarge 成为应用级 `MediaReadResult`，原始 FFmpeg 码为 0，不伪造一个协议错误。直播分类器把 Full 映射为可恢复 QueueLimit，把单包过大映射为致命 PacketTooLarge；本地与点播不进入直播重连规则。Full 先启动原恢复预算，再中止两个队列、接回 worker 和关闭输入；次数/45 秒上限、可取消退避均沿用 3.2。音频停止导致视频等待主时钟时，视频队列可能先积压到跨度限额，因此可以在音频 10 秒期限之前以明确的 Full 原因重建。

最后 GUI 通过 `getPlaybackSnapshot()` 取得队列锁内一致的 stats 副本，不调用网络或 WASAPI。Opening 会隔离旧连接的统计；当前/峰值按连接重置，Full 和超大单包按用户请求累计并跨自动尝试保留，手动重新打开再归零。`updateMediaInfoPanel()` 展示这些值及触发维度；超大单包在低延迟模式提示重新以稳定模式打开，稳定模式仍超限则提示不支持负载。队列 payload 限额不覆盖已经在 FFmpeg 解析、解码器、在途帧或设备中的内存。

测试从 `QueueTests` 的三种限界、时间戳异常、引用计数、并发原子入队与两类等待者唤醒开始，再到 `ControllerTests` 的真实 RTSP 单轨突发、共享预算耗尽、Full 退避取消和 FU-A 重组超 16 MiB 单包。旧正常播放夹具改为限界内供包，超限突发保留专门用例，避免用放宽生产阈值让旧测试通过。

---

## 5. OpenGL视频渲染

### 5.1 渲染管线

```
顶点数据 → 顶点着色器 → 光栅化 → 片段着色器 → 帧缓冲
  (VBO)    (变换位置)    (生成像素) (计算颜色)   (显示)
```

### 5.2 代码学习路线

**第一步**：理解 `VideoRenderer.cpp` 的 `createGeometry()`

```cpp
// 全屏四边形：4个顶点覆盖整个屏幕
// NDC坐标范围：[-1, 1]
float vertices[] = {
    // 位置(x,y,z)     // 纹理坐标(s,t)
     1.0f,  1.0f, 0,   1.0f, 0.0f,  // 右上
     1.0f, -1.0f, 0,   1.0f, 1.0f,  // 右下
    -1.0f, -1.0f, 0,   0.0f, 1.0f,  // 左下
    -1.0f,  1.0f, 0,   0.0f, 0.0f,  // 左上
};
```

**第二步**：理解纹理上传

```cpp
// 每帧视频数据上传到GPU纹理
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
    GL_RGB, GL_UNSIGNED_BYTE, data);
```

**第三步**：学习片段着色器特效

打开 `VideoRenderer.cpp` 中的 `fragment_shader_source`，逐个特效阅读：

```glsl
// 灰度效果 - 最简单的特效
float gray = dot(color.rgb, vec3(0.299, 0.587, 0.114));
// 为什么是这三个数？这是人眼对红绿蓝的感知权重
// 人眼对绿色最敏感(0.587)，红色次之(0.299)，蓝色最弱(0.114)

// 边缘检测 - Sobel算子
// 计算水平和垂直方向的亮度梯度
// 梯度大的地方就是边缘
float gx = -tl - 2.0*l - bl + tr + 2.0*r + br;  // 水平梯度
float gy = -tl - 2.0*t - tr + bl + 2.0*b + br;  // 垂直梯度
float edge = sqrt(gx*gx + gy*gy);                  // 梯度幅值
```

### 5.3 关键OpenGL概念


| 概念      | 作用          | 代码中对应               |
| ------- | ----------- | ------------------- |
| VAO     | 存储顶点属性配置    | `glGenVertexArrays` |
| VBO     | 存储顶点数据      | `glGenBuffers`      |
| Shader  | GPU程序       | `glCreateShader`    |
| Uniform | CPU→GPU参数传递 | `glUniform1i/f`     |
| Texture | 图像数据        | `glGenTextures`     |


---

## 6. 音频处理与可视化

### 6.1 FFT算法学习

**阅读文件**：`AudioVisualizer.cpp` 的 `fft()` 函数

FFT将时域信号（振幅随时间变化）转换为频域信号（振幅随频率变化）：

```
时域：          频域：
  /\  /\        |
 /  \/  \  →   |  |     |
/        \      |__|_____|___
 时间轴          频率轴
```

**关键步骤**：

1. **位反转排列**：重排输入数据，为分治做准备
2. **蝶形运算**：合并两个N/2点DFT为N点DFT
3. **旋转因子**：`W_N^k = e^(-2πik/N)`，复数运算

### 6.2 窗函数

```cpp
// 汉宁窗：使信号两端平滑衰减到0
window_[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (FFT_SIZE - 1)));
```

**为什么需要窗函数？**

- FFT假设输入信号是无限周期的
- 实际只取有限长度，直接截断会产生频谱泄漏
- 窗函数使两端平滑衰减，减少泄漏

### 6.3 频谱柱状图

```cpp
// 将FFT结果分组为bar_count_个频段
// 使用对数缩放：低频段更精细（人耳对低频更敏感）
// dB归一化：20*log10(magnitude)，映射[-60,0]dB → [0,1]
```

---

## 7. MySQL数据库集成

### 7.1 连接流程

**阅读文件**：`MediaDatabase.cpp` 的 `connect()` 函数

```cpp
// 三步连接：
driver_ = sql::mysql::get_mysql_driver_instance();  // 1.获取驱动
connection_ = driver_->connect(url, user, password);  // 2.建立连接
connection_->setSchema(database);                      // 3.选择数据库
```

### 7.2 PreparedStatement防SQL注入

```cpp
// ❌ 危险写法（SQL注入风险）：
stmt->execute("SELECT * FROM media WHERE name = '" + name + "'");

// ✅ 安全写法（参数化查询）：
pstmt = connection_->prepareStatement("SELECT * FROM media WHERE name = ?");
pstmt->setString(1, name);  // 参数自动转义
```

### 7.3 数据库初始化

```bash
# 命令行执行
mysql -u root -p < sql/init_database.sql
```

### 7.4 在代码中启用数据库

编辑 `main.cpp`，取消以下行的注释：

```cpp
window.connectDatabase("127.0.0.1", "root", "你的密码", "media_center");
```

---

## 8. Qt GUI与多线程

### 8.1 为什么需要多线程

视频解码是**CPU密集型**任务，如果在 Qt 主线程（UI线程）中直接解码，界面会卡死。因此必须将解码放到独立线程。

### 8.2 实际线程模型（Qt 版本）

```
Qt 主线程                              播放线程 (playbackThread)
│                                      │
├─ QApplication 事件循环               ├─ av_read_frame() → 读取 AVPacket
├─ 处理鼠标/键盘/菜单事件              ├─ avcodec_send_packet() → 发送给解码器
├─ paintGL() → 渲染 OpenGL 帧          ├─ avcodec_receive_frame() → 获取 AVFrame
├─ update() 触发重绘                   ├─ 如果是音频帧：
│                                      │     swr_convert() → 重采样
│                                      │     atempo filter → 变速（可选）
│                                      │     audio_output_->playPCM() → WASAPI
│                                      │     推送 spectrum 给 AudioVisualizer
│                                      ├─ 如果是视频帧：
│                                      │     sws_scale() → YUV→RGB
│                                      │     video_widget_->setVideoFrame() → 存入缓冲
│                                      └─ 根据 pts 计算 sleep 时间 → 帧率控制
```

### 8.3 线程安全的 UI 更新

**Qt 规定：只能在主线程操作 GUI 控件。**

跨线程更新 UI 的正确方式：

```cpp
// ❌ 错误：直接在子线程修改按钮文字
btn_play_->setText("⏸");  // 可能崩溃！

// ✅ 正确：使用 QMetaObject::invokeMethod 回到主线程
QMetaObject::invokeMethod(this, [this]() {
    btn_play_->setText("⏸");
    statusBar()->showMessage("正在播放: xxx");
}, Qt::QueuedConnection);
```

### 8.4 生命周期互斥 —— 为什么用递归锁

`PlayerController` 的三个方法都需要加锁：

```cpp
bool PlayerController::loadFile(const std::string& filename) {
    std::lock_guard<std::recursive_mutex> lock(lifecycle_mutex_);
    stop();  // stop() 内部也要加同一把锁！
    // ... 打开新文件
}
```

如果用 `std::mutex`，`loadFile()` 持有锁后调用 `stop()`，`stop()` 再尝试加同一把锁 → **死锁**。

`std::recursive_mutex` 允许**同一线程多次加锁**，完美解决此问题。

### 8.5 三道关卡防止并发冲突


| 关卡  | 位置                                     | 作用                  | 机制                                      |
| --- | -------------------------------------- | ------------------- | --------------------------------------- |
| 第1道 | `MainWindow::requestLoadAndPlay()`     | 防止 UI 层快速切换产生并发加载请求 | `std::atomic<int> load_request_id_`     |
| 第2道 | `PlayerController::loadFile/play/stop` | 防止生命周期方法被多线程同时调用    | `std::recursive_mutex lifecycle_mutex_` |
| 第3道 | `PlayerController::playbackThread()`   | 防止旧线程在新文件加载后继续运行    | `std::atomic<int> session_id_`          |


### 8.6 同步机制对比


| 机制                          | 适用场景         | 代码中示例                                | 注意事项                        |
| --------------------------- | ------------ | ------------------------------------ | --------------------------- |
| `std::atomic<bool/int>`     | 简单标志位，无需复杂同步 | `playing_`, `paused_`, `session_id_` | 适合读多写少，无需锁开销                |
| `std::mutex`                | 临界区保护        | `seek_mutex_`, `load_mutex_`         | 注意死锁，尽量缩小锁范围                |
| `std::recursive_mutex`      | 同线程需要重复加锁    | `lifecycle_mutex_`                   | 性能略低于普通 mutex，仅在必须时使用       |
| `QMetaObject::invokeMethod` | 跨线程更新 Qt GUI | `MainWindow` 中的状态栏/按钮更新              | 必须指定 `Qt::QueuedConnection` |


---

## 9. 媒体转码

### 9.1 转码流程

```
源文件 → 解封装 → 解码 → 原始帧 → 缩放/重采样 → 编码 → 封装 → 目标文件
```

### 9.2 转码核心优化 (PTS 换算与兼容性)

**1. 时间戳 (PTS) 正确换算**

```cpp
// 核心逻辑：使用 av_rescale_q 进行输入时间基到编码器时间基的转换
out_frame->pts = av_rescale_q(frame->pts, in_time_base, enc_ctx->time_base);
```

**2. MP4/移动端最大兼容性**
强制视频编码器输出 `YUV420P` 格式，并延迟初始化 `sws_ctx` 到接收首帧时，确保转码生成的 MP4 在所有平台均可正常播放。

### 9.3 使用示例

```cpp
MediaTranscoder transcoder;
transcoder.transcode(
    "input.avi",           // 输入文件
    "output.mp4",          // 输出文件
    1920, 1080,            // 目标分辨率
    "libx264",             // 视频编码器
    "aac",                 // 音频编码器
    4000000,               // 目标码率 4Mbps
    [](float progress) {   // 进度回调
        printf("进度: %.1f%%\n", progress * 100);
    }
);
```

### 9.4 编码参数说明

- **preset**：编码速度 vs 压缩率权衡（ultrafast→slow）
- **CRF**：恒定质量因子（0=无损，18=高质量，28=中等，51=最差）
- **码率**：直接控制输出文件大小

---

## 10. 问题排查与调试思路（实战案例）

### 10.1 编译错误


| 错误信息                            | 原因               | 解决方法                                                          |
| ------------------------------- | ---------------- | ------------------------------------------------------------- |
| `无法打开 avcodec.h`                | FFmpeg头文件路径未配置   | 检查 `vcxproj` → `AdditionalIncludeDirectories`                 |
| `无法解析的外部符号 avformat_open_input` | FFmpeg lib 未链接   | 检查 `AdditionalDependencies` 添加 `avcodec.lib;avformat.lib;...` |
| `glewInit失败`                    | GLEW DLL 不在 PATH | 复制 `glew32.dll` 到 exe 目录                                      |
| Qt 相关编译错误                       | Qt 版本/路径不匹配      | 确认 Qt 6.11 MSVC2022 64-bit，检查 `QTDIR` 环境变量                    |


### 10.2 运行时 DLL 清单

运行前确保以下 DLL 在 exe 目录或 PATH 中：

```
avcodec.dll, avformat.dll, avutil.dll, swscale.dll, swresample.dll, avfilter.dll  ← FFmpeg
glew32.dll                                                                       ← GLEW
Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, Qt6OpenGLWidgets.dll                    ← Qt 6
```

---

### 10.3 播放倍速后音频静音

**排查步骤**：

1. 先确认 `atempo` filter 是否被正确创建：`avfilter_graph_create_filter()` 返回值检查。
2. 检查 filter graph 的输入格式是否与 `SwrContext` 的输出格式一致（采样率、声道布局、采样格式）。
3. 用 `av_log_set_level(AV_LOG_DEBUG)` 查看 FFmpeg 内部日志，观察 filter 是否报错。
4. 在 `1.0x` 时 bypass filter，确认原路径是否正常；若正常则说明问题在 filter 链路。

**学到的知识**：`atempo` 只接受 `AV_SAMPLE_FMT_FLT` 或 `AV_SAMPLE_FMT_FLTP`，如果输入 `S16` 会导致格式不匹配。

### 10.4 切换视频后播放卡顿（1-2 fps）

**排查步骤**：

1. **定位卡顿层级**：是解码慢？还是渲染慢？在 `playbackThread` 中加入计时日志。
2. **检查并发**：在 `loadFile` 入口处打印线程 ID，确认是否有多个线程同时调用。
3. **检查旧线程是否退出**：`stop()` 是否真正 `join()` 了旧线程？`session_id_` 是否生效？
4. **检查音频端点状态**：`AudioOutput` 是否残留了旧文件的采样率/声道数？
5. **尝试控制变量**：先禁用音频（注释掉 `audio_output_->playPCM()`），若只播视频不卡，则问题在音频链路。

**学到的知识**：WASAPI 共享模式下的 `IAudioClient` 对格式变化敏感，切换文件时可能需要重新初始化。

### 10.5 必须手动 pause/play 才能恢复

**排查步骤**：

1. 对比 "正常播放" 和 "卡顿播放" 时的 `PlayerController` 状态变量（`playing_`, `paused_`, `stop_requested_`）。
2. 检查 `playbackThread` 的 `while` 循环条件是否意外提前退出或进入死循环。
3. 在 `pause()` 和 `play()` 中加入状态打印，观察状态机是否进入异常状态。
4. 考虑时间戳问题：新文件的 `pts` 基准是否从 0 开始？如果 `pts` 跳变，帧率控制 sleep 可能计算为负数或极大值。

**学到的知识**：FFmpeg 不同文件的 `pts` 不一定从 0 开始，必须用 `av_q2d(time_base)` 正确转换。

### 10.6 调试技巧速查


| 技巧          | 操作                                                              | 适用场景             |
| ----------- | --------------------------------------------------------------- | ---------------- |
| FFmpeg 详细日志 | `av_log_set_level(AV_LOG_DEBUG)`                                | 解码器/filter 初始化失败 |
| 播放线程 FPS 计数 | 每 1 秒打印已解码帧数                                                    | 量化卡顿程度，区分解码/渲染瓶颈 |
| 状态机打印       | 在 `play/pause/stop/loadFile` 中输出 `playing_/paused_/session_id_` | 状态机异常            |
| 音频隔离测试      | 注释掉 `playPCM()` 调用                                              | 确认问题是否由音频引起      |
| OpenGL 错误检查 | 关键 GL 调用后加 `glGetError()`                                       | 纹理上传失败、着色器编译失败   |
| Qt 信号槽日志    | 重写 `event()` 或连接 `qDebug()`                                     | UI 事件丢失或重复触发     |


---

## 11. 核心概念速查表

### 11.1 FFmpeg 核心结构体


| 结构体               | 作用                 | 生命周期                                               | 本项目中的位置                                              |
| ----------------- | ------------------ | -------------------------------------------------- | ---------------------------------------------------- |
| `AVFormatContext` | 文件格式上下文，管理所有流      | `open()` 创建，`close()` 释放                           | `MediaDecoder::format_ctx_`                          |
| `AVCodecContext`  | 编解码器上下文，保存解码状态     | `open()` 创建，`close()` 释放                           | `MediaDecoder::video_codec_ctx_`, `audio_codec_ctx_` |
| `AVPacket`        | 压缩数据包（从文件读取的原始数据）  | `av_packet_alloc()` / `av_packet_unref()`          | `MediaDecoder::readPacket()`                         |
| `AVFrame`         | 解码后的原始帧（YUV / PCM） | `av_frame_alloc()` / `av_frame_free()`             | 每次解码后临时创建                                            |
| `SwsContext`      | 视频色彩空间转换上下文        | `sws_getContext()` / `sws_freeContext()`           | `MediaDecoder::sws_ctx_`                             |
| `SwrContext`      | 音频重采样上下文           | `swr_alloc_set_opts()` / `swr_free()`              | `MediaDecoder::swr_ctx_`                             |
| `AVFilterGraph`   | 音频滤波图（`atempo` 链路） | `avfilter_graph_alloc()` / `avfilter_graph_free()` | `MediaDecoder::filter_graph_`                        |


### 11.2 Qt 核心类


| 类                           | 作用                    | 本项目用法                                       |
| --------------------------- | --------------------- | ------------------------------------------- |
| `QApplication`              | Qt 应用程序入口，事件循环        | `main.cpp` 中创建                              |
| `QMainWindow`               | 主窗口，含菜单栏/工具栏/状态栏/停靠窗口 | `MainWindow` 继承                             |
| `QOpenGLWidget`             | 带 OpenGL 上下文的 QWidget | `VideoWidget` 继承，用于渲染视频                     |
| `QTimer`                    | 定时器，触发周期性事件           | `VideoWidget` 中 16ms 触发 `update()` 实现 60fps |
| `QMetaObject::invokeMethod` | 跨线程调用槽函数              | `MainWindow` 中子线程更新 UI                      |
| `QThread` / `std::thread`   | 多线程                   | `MainWindow` 用 `std::thread(detach)` 做异步加载  |


### 11.3 OpenGL 核心对象


| 对象      | 作用             | 创建/销毁函数                                      |
| ------- | -------------- | -------------------------------------------- |
| VAO     | 存储顶点属性配置       | `glGenVertexArrays` / `glDeleteVertexArrays` |
| VBO     | 存储顶点数据         | `glGenBuffers` / `glDeleteBuffers`           |
| EBO     | 存储索引数据         | `glGenBuffers` / `glDeleteBuffers`           |
| Shader  | GPU 程序         | `glCreateShader` / `glDeleteShader`          |
| Program | 着色器程序（顶点+片段链接） | `glCreateProgram` / `glDeleteProgram`        |
| Texture | 图像/视频帧数据       | `glGenTextures` / `glDeleteTextures`         |
| FBO     | 帧缓冲对象（离屏渲染）    | `glGenFramebuffers` / `glDeleteFramebuffers` |


### 11.4 关键设计模式


| 模式   | 本项目应用                                       | 解决的问题               |
| ---- | ------------------------------------------- | ------------------- |
| 请求去重 | `load_request_id_` + `requestLoadAndPlay()` | 连续切换媒体时避免旧加载结果覆盖新请求 |
| 单例   | `sql::mysql::get_mysql_driver_instance()`   | MySQL 驱动全局唯一        |
| 状态机  | `PlayerController` (playing/paused/stopped) | 播放生命周期管理            |
| 命令队列 | `MainWindow::requestLoadAndPlay()`          | 异步操作序列化、去重          |
| 观察者  | Qt 信号槽机制                                    | UI 与逻辑解耦            |
