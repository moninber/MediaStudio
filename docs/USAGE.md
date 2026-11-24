# MediaStudio 使用文档

## 目录

1. [简介](#1-简介)
2. [系统要求](#2-系统要求)
3. [环境准备](#3-环境准备)
4. [编译步骤](#4-编译步骤)
5. [运行程序](#5-运行程序)
6. [功能说明](#6-功能说明)
7. [键盘快捷键](#7-键盘快捷键)
8. [数据库配置](#8-数据库配置)
9. [常见问题](#9-常见问题)
10. [技术支持](#10-技术支持)

---

## 1. 简介

MediaStudio 是一个基于 C++17 的桌面音视频播放器，采用 Qt 6.11 + OpenGL + FFmpeg + WASAPI 技术栈。

### 核心功能

- **视频播放**：支持多种视频格式（MP4, MKV, AVI, FLV 等）
- **音频播放**：支持多种音频格式（MP3, AAC, FLAC, WAV 等）
- **播放控制**：播放/暂停、停止、进度跳转、音量调节、倍速播放（0.25x - 4.0x）
- **视频特效**：OpenGL 着色器特效（正常、灰度、边缘检测、反色、模糊、亮度/对比度、怀旧）
- **音频可视化**：FFT 频谱分析，实时显示音频频谱柱状图
- **播放列表**：支持添加/删除文件、双击播放、上一首/下一首
- **媒体库**：MySQL 数据库存储媒体信息、收藏功能、播放历史（CMake/MinGW 默认关闭）
- **格式转码**：FFmpeg 编码 API 支持格式转换

---

## 2. 系统要求

### 硬件要求

- **操作系统**：Windows 10/11（64 位）
- **CPU**：支持 SSE2 指令集（Intel Core 2 Duo 或更高）
- **内存**：4GB RAM 以上（推荐 8GB）
- **显卡**：支持 OpenGL 3.3 Core Profile

### 软件要求

- **构建系统**：CMake + Ninja
- **编译器**：Qt 自带 MinGW 13.1.0（默认）
- **C++ 标准**：C++17
- **平台**：x64

---

## 3. 环境准备

### 3.1 必需软件

#### Qt 6.11

安装 Qt 6.11，并勾选 MinGW 64-bit、CMake、Ninja 组件。推荐保证 `cmake`、`ninja` 和 MinGW 编译器已经加入 `PATH`，或通过本地 `CMakeUserPresets.json` 显式指定。

```text
<qt-root>\
<mingw-root>\
<cmake-executable>
<ninja-executable>
```

#### FFmpeg 8.0

下载 FFmpeg 开发库并解压到本地目录：

```text
<ffmpeg-root>\
├── include\          ← 头文件（avcodec.h, avformat.h 等）
├── lib\              ← 库文件（*.lib / *.dll.a）
└── bin\              ← DLL 文件（运行时需要）
```

#### GLEW 2.3.1

下载 GLEW 并解压到本地目录：

```text
<glew-root>\
├── include\
├── lib\Release\x64\
└── bin\Release\x64\
    └── glew32.dll
```

#### MariaDB Connector/C（可选）

CMake/MinGW 构建默认关闭 MySQL：

```text
MEDIASTUDIO_ENABLE_MYSQL=OFF
```

如果需要启用数据库，请准备与当前编译器 ABI 兼容的 MariaDB Connector/C 或 MySQL C API 兼容库，并记录其本地路径：

```text
<mysql-connector-root>\
<mysql-server-root>\
```

---

## 4. 编译步骤

### 4.1 一键编译（推荐）

在项目根目录执行。若本地存在 `CMakeUserPresets.json`，脚本会优先使用 `mingw-release-local`；否则使用公开的 `mingw-release`：

```powershell
.\tools\build-mingw.ps1
```

脚本会自动：

- 使用当前环境中的 `cmake`
- 生成 Release 构建
- 在配置了依赖路径后调用 `windeployqt`
- 复制 FFmpeg/GLEW 运行时 DLL 到输出目录
- 检查 CMake 退出码，配置或构建失败时立即停止

输出文件随所选预设确定：

| 预设 | 输出文件 |
| --- | --- |
| 本机 `mingw-release-local` | `build\mingw-release-local\MediaStudio.exe` |
| 公共 `mingw-release` | `build\mingw-release\MediaStudio.exe` |

### 4.2 分步编译

```powershell
.\tools\configure-mingw.ps1
.\tools\build-mingw.ps1
```

项目目录移动、改名或缓存仍指向旧路径时，显式重新生成当前预设的缓存：

```powershell
.\tools\build-mingw.ps1 -Fresh
```

只配置而不构建时，使用 `.\tools\configure-mingw.ps1 -Fresh`。日常增量构建无需 `-Fresh`；重新生成缓存时以预设为准，本机额外配置应先写入 `CMakeUserPresets.json`。编译命令数据库 `compile_commands.json` 会随配置更新，其他预设的构建目录不受本次操作影响。

### 4.3 手动 CMake 命令

```powershell
cmake --preset mingw-release `
  -DMEDIASTUDIO_QT_ROOT=<path-to-qt> `
  -DMEDIASTUDIO_FFMPEG_ROOT=<path-to-ffmpeg> `
  -DMEDIASTUDIO_GLEW_ROOT=<path-to-glew>

cmake --build --preset mingw-release
```

### 4.4 Debug 构建

```powershell
cmake --preset mingw-debug `
  -DMEDIASTUDIO_QT_ROOT=<path-to-qt> `
  -DMEDIASTUDIO_FFMPEG_ROOT=<path-to-ffmpeg> `
  -DMEDIASTUDIO_GLEW_ROOT=<path-to-glew>

cmake --build --preset mingw-debug
```

### 4.5 运行播放基础测试

构建后，对所用预设的构建目录执行 CTest。例如本机 Release：

```powershell
ctest --test-dir build/mingw-release-local --output-on-failure
```

测试使用临时本地音频和回环 RTSP 服务，检查输入策略、快照、读结果及阻塞读取的取消和超时。无需启动虚拟机或准备视频素材；只构建播放器时可用 `-DBUILD_TESTING=OFF` 关闭测试目标。

## 5. 运行程序

### 5.1 使用脚本运行

```powershell
.\tools\run-mingw.ps1
```

带媒体文件参数：

```powershell
.\tools\run-mingw.ps1 "D:\Videos\test.mp4"
```

### 5.2 直接运行

双击或命令行运行所选预设的产物。本机预设对应：

```text
build\mingw-release-local\MediaStudio.exe
```

公共预设对应：

```text
build\mingw-release\MediaStudio.exe
```

然后通过菜单 **文件 → 打开** 加载媒体。

`run-mingw.ps1` 只在产物缺失时调用构建；已有 EXE 时直接运行。修改源码后应先执行构建脚本。

### 5.3 拖拽运行

直接将视频文件拖拽到 MediaStudio 窗口即可播放。

---

## 6. 功能说明

### 6.1 播放控制

| 功能 | 操作方式 | 说明 |
|------|---------|------|
| 播放/暂停 | 点击 ▶/⏸ 按钮或按 Space 键 | 切换播放状态 |
| 停止 | 点击 ■ 按钮 | 停止当前媒体，保留窗口和播放列表 |
| 退出 | 按 Esc 键或关闭窗口 | 取消当前任务并等待后台收尾 |
| 进度跳转 | 拖动进度条 | 跳转到指定位置 |
| 音量调节 | 拖动音量滑块或按 ↑/↓ 键 | 调节音量（0-100%） |
| 倍速播放 | 下拉框选择全部档位，或按 1-4 数字键快速切换常用档位 | 数字键对应 0.5x, 1.0x, 1.5x, 2.0x |
| 静音/取消静音 | 按 M 键 | 快速切换静音状态 |
| 上一首 | 点击 ◀◀ 按钮或按 PageUp 键 | 播放列表上一首 |
| 下一首 | 点击 ▶▶ 按钮或按 PageDown 键 | 播放列表下一首 |

### 6.2 视频特效

通过菜单 **视图 → 切换特效** 或按 E 键切换特效：

1. **原始**：原始画面
2. **灰度**：黑白效果
3. **反色**：颜色反转
4. **边缘检测**：Sobel 算子边缘检测
5. **模糊**：盒式模糊
6. **亮度对比度**：调节亮度和对比度
7. **怀旧色调**：复古色调

### 6.3 音频可视化

通过菜单 **视图 → 切换频谱** 或按 V 键开启/关闭音频频谱可视化。

### 6.4 播放列表

- **添加文件**：菜单 **文件 → 添加到播放列表** 或拖拽文件到播放列表
- **添加 RTSP 直播**：菜单 **文件 → 打开网络媒体…**；提交后添加到同一列表并开始连接
- **删除列表项**：选中播放列表项后点击“➖ 移除”按钮
- **播放列表项**：双击文件或直播项，也可通过上一首/下一首切换
- **清空列表**：点击“🗑 清空”按钮

### 6.5 媒体信息

右侧停靠面板显示当前媒体的详细信息：

- 文件名
- 时长
- 视频编码、分辨率、帧率
- 音频编码、采样率、声道数
- 比特率

直播显示协议、传输方式、预设与已识别的音视频参数，不显示总时长和文件大小。

### 6.6 数据库功能（可选）

CMake/MinGW 默认构建中数据库功能关闭。此时菜单仍存在，但连接会提示“数据库功能未启用”。

如果启用 MySQL 构建：

1. 菜单 **工具 → 连接数据库**
2. 输入 MySQL 连接信息：
   - 主机地址：127.0.0.1
   - 用户名：root
   - 密码：你的 MySQL 密码
   - 数据库名：media_center
3. 点击确定

#### 收藏功能

- 按 F 键切换当前媒体的收藏状态
- 收藏的媒体在数据库中标记为 `is_favorite = 1`
- 播放列表中收藏的媒体会有特殊标识

#### 播放历史

- 每次播放自动记录到 `play_history` 表
- 包含播放时间戳和播放时长

### 6.7 格式转码（可选）

菜单 **工具 → 格式转码**：

1. 选择输入文件
2. 选择输出文件
3. 设置目标分辨率、编码器、码率
4. 点击开始转码

### 6.8 多线程播放架构（技术说明）

当前播放器采用三线程播放架构，以提升性能和音视频同步稳定性：

#### 线程架构

- **demux/control 线程**：读取压缩包并分发到音频/视频队列
- **音频工作线程**：从队列取包 → 解码 → 播放 → 更新音频主时钟
- **视频工作线程**：从队列取包 → 解码 → 根据音频主时钟同步 → 渲染

#### generation 机制（车次号）

为解决 seek/变速时的状态混乱，引入 generation 机制：

- **类比**：generation = 车次号，packet/frame = 乘客，音频/视频线程 = 接站的人
- **seek/变速时**：`stream_generation.fetch_add(1)` 换车次
- **旧包处理**：队列里的旧车次包被丢弃（`packet_generation != stream_generation`）
- **效果**：旧速度的包不会用新速度解码，避免音频忽快忽慢

#### 状态栏调试信息

播放器状态栏右侧会显示渲染/解码 FPS、A/V diff、累计丢帧、音频/视频队列和音频缓冲，主要用于快速判断播放是否稳、卡在哪里。

当前 FPS 前值统计提交给 `VideoWidget` 的视频帧，后值统计解码帧，不是屏幕实际绘制帧率。A/V diff 在同步等待前更新；播放停滞时可能保留旧值。Drop 是视频同步丢帧，队列值为包数，Buf 为声卡当前排队音频时长，均不能直接当作网络丢包或端到端延迟。

#### 音频主时钟

- **WASAPI 播放进度**：`audio_output_->getPlayedSeconds()` 返回已播放秒数
- **时钟计算**：`audio_clock_base + playedSeconds * active_speed`
- **视频同步**：视频帧根据音频主时钟等待/丢帧
- **纯视频文件**：无音频时使用单调时钟控制节奏；纯音频仍按音频输出时钟推进

### 6.9 打开 RTSP 直播

1. 选择 **文件 → 打开网络媒体…**，输入 RTSP 地址，例如 `rtsp://127.0.0.1:8554/live`。输入框遮盖地址，以保护可能随地址输入的凭据。
2. 选择 TCP 或 UDP。默认使用 TCP；当前入口只提供稳定模式。“自动重连”默认勾选，可为这一播放项关闭。点击打开后，状态栏显示连接状态，列表用安全标签区分各路直播。
3. 点击播放/暂停按钮或按 **Space** 暂停直播：播放器停止声音并断开连接，保留最后已经绘制的视频画面；状态先显示正在断开，收尾完成后显示已断开。再次点击或按 Space 会重新连接最新直播，重新等待关键帧，不补播暂停期间的内容。连接、缓冲或自动重连等待中也可以暂停取消。
4. 点击工具栏的 **回到直播** 重新连接当前直播。音量、特效、频谱视图和当前源的 TCP/UDP、自动重连设置均保留；反复点击会取消前一次操作，以最后一次操作为准。暂停中或连接失败后也可使用。停止后可从列表重新打开。
5. 直播进度条定位、方向键快进快退和倍速不可用；切回本地文件后恢复文件操作和之前的文件倍速。本地文件仍在原位置暂停/继续。

连接完成后，状态栏先显示正在缓冲。媒体信息区分别显示首包、首段 PCM、首帧提交和首帧绘制；双轨直播要等声音和视频都就绪才显示播放。纯音频或纯视频只需自己的轨道就绪。使用频谱视图时，首帧绘制可以继续显示“—”，这不影响出声或播放状态。

启用自动重连时，可恢复断流会先释放旧音视频线程、声卡和输入，再按 1、2、4 秒退避，最多启动 3 次额外连接；本轮恢复总计最多 45 秒。状态栏显示重试倒计时，媒体信息显示连接尝试、累计自动重试、本轮已用次数和最近故障。访问被拒绝等明确错误、次数或时间耗尽、关闭自动重连时进入播放失败；此时仍可点击“回到直播”手动发起新请求。暂停、停止、切源或关窗会取消退避和已开始的新连接，不会后台复活。

自动连接沿用本次用户请求，但重新建立解码器、元信息和音画时钟；四个首次事件都从当前连接开始重新计时。断流等待时保留的画面和事件属于历史，轨道显示已停止输出；新连接只有实际写入 PCM 和提交所需视频帧后才算恢复。各活动轨道连续 10 秒没有有效输出也会触发恢复，另一轨仍收包不能延长它的期限；若另一轨已经积压到队列限界，Full 可以更早触发恢复；频谱模式没有视频绘制不会单独触发恢复。连续 30 秒健康输出后恢复本轮可用预算，累计自动重试次数保留。

稳定模式对音频、视频队列分别执行限界：各最多 512 包，音频 payload 最多 2 MiB、视频最多 16 MiB，可信时间戳首尾跨度最多 2 秒。任一轨下一包将超限时返回 Full，结束这一连接并共用上述有限恢复预算，不静默丢压缩包或无限等待。单包本身超过字节限额时直接失败，不反复重连同一负载；超过稳定模式限额会提示不支持该负载。限额只约束排队 payload，不是播放器总内存上限。

媒体信息显示每轨当前/上限、payload、可信跨度、包数/字节峰值，以及按本次用户请求累计的 Full 和超大单包次数。空队列、缺失时间戳或时间域跳变时跨度显示“不可用”，包数和字节限界仍有效；这些事件与 A/V 同步丢帧、损坏帧跳过分别统计，不能当作网络丢包率。

网络项仅保存在本次运行的播放列表中，不进入媒体库或播放历史。当前 UI 仍只开放稳定 RTSP；低延迟完整配置、HTTP/HTTPS 点播、RTMP 和 SRT 入口将在后续阶段接入，本轮不据此宣称弱网或耐久验收通过。

---

## 7. 键盘快捷键

| 快捷键 | 功能 |
|--------|------|
| **Space** | 播放/暂停 |
| **Esc** | 退出程序 |
| **↑ / ↓** | 音量 + / - |
| **← / →** | 快退 / 快进 5 秒 |
| **PageUp / PageDown** | 上一首 / 下一首 |
| **1 - 4** | 切换倍速（0.5x, 1.0x, 1.5x, 2.0x） |
| **E** | 切换视频特效 |
| **V** | 切换音频可视化 |
| **F** | 切换收藏状态（需连接数据库） |
| **M** | 静音/取消静音 |
| **O** | 打开文件 |

---

## 8. 数据库配置

### 8.1 安装 MySQL

下载并安装 MySQL 8.0 Community Server：

```text
https://dev.mysql.com/downloads/mysql/
```

### 8.2 创建数据库

运行 `sql/init_database.sql`：

```bash
mysql -u root -p < sql/init_database.sql
```

### 8.3 启用 CMake 数据库构建

默认 CMake/MinGW 构建关闭数据库：

```text
MEDIASTUDIO_ENABLE_MYSQL=OFF
```

如果已经准备好兼容 MinGW 的 MySQL/MariaDB Connector，可以重新配置：

```powershell
cmake -S . -B build\mingw-release -G Ninja `
  -DCMAKE_PREFIX_PATH=<path-to-qt> `
  -DMEDIASTUDIO_QT_ROOT=<path-to-qt> `
  -DMEDIASTUDIO_ENABLE_MYSQL=ON `
  -DMEDIASTUDIO_MYSQL_CONNECTOR_ROOT=<path-to-mariadb-connector> `
  -DMEDIASTUDIO_MYSQL_SERVER_ROOT=<path-to-mysql-server>
```

### 8.4 在代码中自动连接数据库

编辑 `main.cpp`，取消以下行的注释并填入密码：

```cpp
window.connectDatabase("127.0.0.1", "root", "your_password", "media_center");
```

也可以不修改代码，在程序中通过菜单 **工具 → 连接数据库** 手动连接。

---

## 9. 常见问题

### 9.1 编译错误

| 错误信息 | 原因 | 解决方法 |
|---------|------|---------|
| `cmake 不是可识别的命令` | 系统 PATH 没有 CMake | 将 CMake 加入 PATH，或改用本机绝对路径执行 |
| `ninja 不是可识别的命令` | 系统 PATH 没有 Ninja | 将 Ninja 加入 PATH，或改用本机绝对路径执行 |
| `无法打开 avcodec.h` | FFmpeg 头文件路径未配置 | 检查 `MEDIASTUDIO_FFMPEG_ROOT` |
| `cannot find -l...` | 库文件路径或 ABI 不匹配 | 检查 FFmpeg/GLEW 是否为 x64 且与编译器兼容 |
| `mysql_driver.h` 找不到 | 数据库依赖未配置 | 默认关闭 MySQL，或设置 `MEDIASTUDIO_ENABLE_MYSQL=ON` 并配置路径 |

### 9.2 运行时错误

| 错误信息 | 原因 | 解决方法 |
|---------|------|---------|
| `找不到 avcodec-xx.dll` | FFmpeg DLL 未复制到 exe 目录 | 重新执行 `tools\build-mingw.ps1` |
| `找不到 Qt6Core.dll` | Qt DLL 未部署 | 重新执行 `tools\build-mingw.ps1`，让 `windeployqt` 部署 |
| `数据库功能未启用` | CMake 默认关闭 MySQL | 使用 `MEDIASTUDIO_ENABLE_MYSQL=ON` 重新配置 |
| `数据库连接失败` | MySQL 未启动或密码错误 | 检查 MySQL 服务状态，确认连接信息 |

### 9.3 播放问题

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| 画面卡顿 | 硬件解码未启用或分辨率过高 | 降低分辨率或更换更轻量的编码格式 |
| 音频不同步 | 倍速滤波器初始化失败 | 尝试 1.0x 倍速，检查 FFmpeg 日志 |
| 切换视频后卡顿 | 音频缓冲区残留 | 手动暂停/播放恢复，或重启程序 |

### 9.4 Qt/GLEW 警告

构建时可能看到：

```text
qopenglfunctions.h is not compatible with GLEW
```

当前项目仍可编译并链接成功。这个警告来自 `QOpenGLFunctions` 与 GLEW 同时使用。后续如果要彻底清理，可以统一改为只使用 Qt OpenGL 函数封装，或只使用 GLEW。

---

## 10. 技术支持

如遇到问题，请检查：

1. `cmake`、`ninja` 和编译器是否可用
2. `MEDIASTUDIO_QT_ROOT`、`MEDIASTUDIO_FFMPEG_ROOT`、`MEDIASTUDIO_GLEW_ROOT` 是否正确
3. Qt、FFmpeg、GLEW 是否与当前编译器和平台架构兼容
4. 当前预设的输出目录（`build\mingw-release-local` 或 `build\mingw-release`）中是否已经部署运行时 DLL
