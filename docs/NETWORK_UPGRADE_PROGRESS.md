# 网络能力改造进度与验证记录

执行计划以 `E:\Learn\docs\MediaStudio_网络流能力改造_交接.md` 为准。本文记录实际结果，不把依赖支持或启动成功当作网络播放验收。

## Git 交付规则

用户于 2026-11-06 确认：项目改动必须使用 Git 保留回退点；每个编号完成并经用户确认后，向远端推送一次。当前工作分支为 `codex/network-streaming`，起点为 `212e472d6d15309499f4c06b0b2b62f07e7ab2d2`。本机预设、构建产物和原始实验日志不提交。

## 任务状态

| 编号 | 状态 | 说明 |
| --- | --- | --- |
| 0.1 | 技术核验完成，待用户确认后推送 | 新目录配置、构建、空启动、正常关窗、运行库及协议能力已核验 |
| 0.2 | 待做 | 本地音画、seek、倍速、输出及资源基线 |
| 0.3 | 待做 | VMware、推流与受控弱网测试条件 |
| 1.1～8.4 | 待做 | 按交接稿逐项推进 |

## 0.1：目录迁移后的构建与启动基线

核验时间：2026-11-06，Asia/Shanghai。原始证据目录：`E:\Learn\demos\network-player-validation\0.1-20261106`。

### 改动与保留项

- 在修改前建立 `codex/network-streaming` 分支；原工作区干净。
- 新增 `AGENTS.md`，保存逐编号、本地提交、用户确认后推送的协作规则。
- `configure-mingw.ps1` 与 `build-mingw.ps1` 增加显式 `-Fresh` 开关；仅为所选预设重新生成 CMake 缓存。
- 配置和构建均检查 CMake 的原生退出码；配置失败不继续构建，构建失败不报告成功，异常时恢复调用前的工作目录。
- 公共 Release/Debug 预设显式开启 `CMAKE_EXPORT_COMPILE_COMMANDS`。旧 local-release 缓存原先单独设置了此值，单用 `--fresh` 会丢失该设置并留下旧 `compile_commands.json`；写入预设后重新生成，不手改生成文件中的绝对路径。
- 未修改播放器源码、`CMakeLists.txt`、本机预设、数据库 schema 或 `SUMMARY.md`。其他三个构建目录未重新配置，其旧缓存仍不适合直接使用。

### 本次有效构建配置

| 项目 | 实际值 |
| --- | --- |
| 预设 / 生成器 | `mingw-release-local` / Ninja |
| CMake | `3.31.6-msvc6` |
| 编译器 | Qt 配套 MinGW GCC/G++ `13.1.0` |
| Qt | `E:\Qt\6.11.0\mingw_64` |
| FFmpeg | `E:\ffmpeg`，部署库版本 `8.0.1-full_build-www.gyan.dev` |
| GLEW | `E:\OpenGl\glew-2.3.1-win32\glew-2.3.1`，实际链接 `glew32.lib` |
| 数据库 | `MEDIASTUDIO_ENABLE_MYSQL=ON`，连接器 `E:\DevTools\mariadb-mingw64` |
| 构建类型 / 运行库部署 | Release / ON |
| 源码 / 构建目录 | `E:\Learn\MediaStudio` / `build\mingw-release-local` |
| 程序 | `E:\Learn\MediaStudio\build\mingw-release-local\MediaStudio.exe` |

数据库开关延续本机预设，没有切回公共预设的 OFF。程序入口的自动数据库连接代码目前处于注释状态，因此本次未建立数据库连接或验证 CRUD。

本机 `CMakeUserPresets.json` 修改前后 SHA-256 均为 `8AFD0D7C2679C07271227303E897E14C164A16719FBE2464846AB02ECFEC5490`。编译器、依赖根、数据库与部署开关均已逐值对照原缓存；CMake 缓存条目的类型标记可能重新推导，不影响这些值。

复现构建命令：

```powershell
Set-Location E:\Learn\MediaStudio
.\tools\build-mingw.ps1 -Fresh
```

日常增量构建使用 `.\tools\build-mingw.ps1`，无需每次重建缓存。`-Fresh` 会根据当前预设重新建立缓存；额外的本机配置应写入被忽略的本机预设后再使用此开关。

### 验证结果

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| 全新配置、编译、链接和运行库部署 | 14/14 构建步骤完成，命令成功退出 | `build-final.log` |
| 旧项目路径 | 所选目录的 cache、Ninja、CMake 脚本及编译命令数据库未匹配旧绝对路径 | `configured-cache.txt`；生成文件复核 |
| 编译命令数据库 | 重新生成 12 个编译条目，目录为当前项目 | `build\mingw-release-local\compile_commands.json` |
| 脚本失败传播 | 配置非零会失败；构建入口在配置失败后不调用 build；构建非零会失败；`-Fresh` 正确传递，工作目录恢复 | `script-failure-checks.json`，使用受控 CMake 替身隔离验证 |
| 启动与窗口 | 新产物 PID 12724，主窗口显示“就绪”，OpenGL、渲染器和频谱初始化成功 | `startup.stdout.log`、`window-accessibility.txt` |
| OpenGL | `4.6.0 NVIDIA 621.01` | `startup.stdout.log` |
| 启动错误与退出 | 标准错误为空，通过窗口关闭按钮正常关闭后进程不再存在 | `startup.stderr.log`、`startup-checks.json` |
| 实际加载路径 | 播放器的六个 FFmpeg DLL、Qt、GLEW、MariaDB 和 MinGW runtime 均从本次构建目录加载 | `loaded-modules.json` |
| FFmpeg 部署一致性 | 七个部署 DLL 与 `E:\ffmpeg\bin` 对应文件的 SHA-256 全部一致 | `deployed-ffmpeg.json` |
| 部署库能力 | 直接加载该目录 DLL，枚举得到 HTTP/HTTPS/TLS/RTMP/SRT/TCP/UDP/RTP 输入协议及 RTSP/MOV/Matroska/MPEG-TS demuxer，启用 GnuTLS/libsrt | `probe-deployed-ffmpeg.py`、`deployed-ffmpeg.json` |
| 数据库构建能力 | EXE 导入并实际加载 `libmariadb.dll`，版本 `3.4.8.0` | `exe-imports.txt`、`loaded-modules.json` |

七个 FFmpeg 库包括 `avdevice`；当前 EXE 的实际导入与进程模块中只有其余六个，不能将部署/探针加载表述为播放器加载了全部七个。协议枚举证明依赖能力，不证明播放器已实现取消、重连、直播或点播行为。

当前项目未直接链接 `Qt6::Network`。Qt Network 开发文件及 TLS 插件已存在，部署工具因 GUI 插件依赖复制 Qt6Network；空启动未加载该模块，TLS 后端和 HTTPS 校验仍需在阶段 6 实测。本机 FFmpeg TLS 帮助显示 `tls_verify` 默认 0，后续必须显式配置并验证。

### 既有警告与未覆盖项

- Qt/GLEW 头文件兼容性警告；本次 OpenGL 初始化成功，未在 0.1 重构渲染模块。
- 转码器使用的 `AVCodec::pix_fmts/sample_fmts` 被 FFmpeg 标记弃用。
- `MediaDecoder` 的滤镜 flush 路径未检查 `av_buffersrc_add_frame_flags` 返回值。
- CMake 提示未找到可选 Vulkan 头文件；部署工具提示缺少 `dxcompiler.dll/dxil.dll`。本次 OpenGL 空启动通过，未据此安装无关组件。
- 其余三个旧构建目录仍保留迁移前缓存；旧 debug/debug-local 的数据库开关还与各自预设不同。只有本次选定的 local-release 配置被重新生成和验收。
- `run-mingw.ps1` 在 EXE 已存在时直接运行，不保证自动重建；本次使用显式构建后的产物验证。
- detached 加载线程、不可取消 I/O、共享读包锁、队列上界等网络改造前缺口仍在，留待后续对应编号处理。

0.1 没有验证媒体解码、WASAPI 出声、seek/倍速、数据库业务、虚拟机或网络拉流。外部证据目录已因本次构建记录需要提前创建，不代表 0.2 完成。

### 阅读顺序与下一项

先读本文的配置与结果，再看 `tools/configure-mingw.ps1`、`tools/build-mingw.ps1` 和 `CMakePresets.json`。下一编号为 0.2；0.1 经用户确认后再推送本编号提交，不自动进入下一项。
