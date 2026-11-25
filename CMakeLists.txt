cmake_minimum_required(VERSION 3.24)

project(MediaStudio VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MEDIASTUDIO_ENABLE_MYSQL "Enable MySQL media library support" OFF)
option(MEDIASTUDIO_DEPLOY_RUNTIME "Copy runtime DLLs after build" ON)

set(MEDIASTUDIO_QT_ROOT "" CACHE PATH "Qt installation root")
set(MEDIASTUDIO_FFMPEG_ROOT "" CACHE PATH "FFmpeg installation root")
set(MEDIASTUDIO_GLEW_ROOT "" CACHE PATH "GLEW installation root")
set(MEDIASTUDIO_MYSQL_CONNECTOR_ROOT "" CACHE PATH "MySQL Connector/C root")
set(MEDIASTUDIO_MYSQL_SERVER_ROOT "" CACHE PATH "MySQL Server root")

if(EXISTS "${MEDIASTUDIO_QT_ROOT}")
    list(APPEND CMAKE_PREFIX_PATH "${MEDIASTUDIO_QT_ROOT}")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
find_package(OpenGL REQUIRED)

set(MEDIASTUDIO_PLAYBACK_SOURCES
    src/PlaybackSource.cpp
    src/MediaIo.cpp
    src/LiveRecovery.cpp
    src/MediaDecoder.cpp
)

set(MEDIASTUDIO_SESSION_SOURCES
    src/LiveReceiveMonitor.cpp
    src/LiveBacklog.cpp
    src/AudioOutput.cpp
    src/VideoRenderer.cpp
    src/AudioVisualizer.cpp
    src/PacketQueue.cpp
    src/PlayerController.cpp
    src/VideoWidget.cpp
)

set(MEDIASTUDIO_SOURCES
    src/main.cpp
    ${MEDIASTUDIO_PLAYBACK_SOURCES}
    ${MEDIASTUDIO_SESSION_SOURCES}
    src/MediaTranscoder.cpp
    src/MainWindow.cpp
)

set(MEDIASTUDIO_HEADERS
    src/PlaybackSource.h
    src/PlaybackSnapshot.h
    src/LiveReceiveMonitor.h
    src/LiveBacklog.h
    src/MediaIo.h
    src/LiveRecovery.h
    src/MediaInfo.h
    src/MediaDecoder.h
    src/AudioOutput.h
    src/AudioBufferSettings.h
    src/VideoRenderer.h
    src/AudioVisualizer.h
    src/MediaTranscoder.h
    src/PacketQueue.h
    src/MediaDatabase.h
    src/PlayerController.h
    src/VideoWidget.h
    src/MainWindow.h
)

if(MEDIASTUDIO_ENABLE_MYSQL)
    list(APPEND MEDIASTUDIO_SOURCES src/MediaDatabase.cpp)
endif()

qt_standard_project_setup()
qt_add_executable(MediaStudio WIN32
    ${MEDIASTUDIO_SOURCES}
    ${MEDIASTUDIO_HEADERS}
)

target_compile_definitions(MediaStudio PRIVATE
    QT_OPENGLWIDGETS_LIB
    WIN32_LEAN_AND_MEAN
    MEDIASTUDIO_ENABLE_MYSQL=$<BOOL:${MEDIASTUDIO_ENABLE_MYSQL}>
)

target_include_directories(MediaStudio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MEDIASTUDIO_FFMPEG_ROOT}/include"
    "${MEDIASTUDIO_GLEW_ROOT}/include"
)

function(mediastudio_import_unknown_library target_name library_path)
    if(NOT EXISTS "${library_path}")
        message(FATAL_ERROR "Required library not found: ${library_path}")
    endif()
    add_library(${target_name} UNKNOWN IMPORTED)
    set_target_properties(${target_name} PROPERTIES IMPORTED_LOCATION "${library_path}")
endfunction()

set(MEDIASTUDIO_FFMPEG_COMPONENTS
    avcodec
    avformat
    avdevice
    avutil
    avfilter
    swresample
    swscale
)

set(MEDIASTUDIO_FFMPEG_TARGETS)
foreach(component IN LISTS MEDIASTUDIO_FFMPEG_COMPONENTS)
    if(MINGW AND EXISTS "${MEDIASTUDIO_FFMPEG_ROOT}/lib/lib${component}.dll.a")
        set(component_library "${MEDIASTUDIO_FFMPEG_ROOT}/lib/lib${component}.dll.a")
    elseif(EXISTS "${MEDIASTUDIO_FFMPEG_ROOT}/lib/${component}.lib")
        set(component_library "${MEDIASTUDIO_FFMPEG_ROOT}/lib/${component}.lib")
    elseif(EXISTS "${MEDIASTUDIO_FFMPEG_ROOT}/lib/lib${component}.dll.a")
        set(component_library "${MEDIASTUDIO_FFMPEG_ROOT}/lib/lib${component}.dll.a")
    else()
        message(FATAL_ERROR "FFmpeg library not found: ${component}")
    endif()

    set(component_target "FFmpeg::${component}")
    mediastudio_import_unknown_library(${component_target} "${component_library}")
    list(APPEND MEDIASTUDIO_FFMPEG_TARGETS ${component_target})
endforeach()

if(MINGW AND EXISTS "${MEDIASTUDIO_GLEW_ROOT}/lib/Release/x64/libglew32.dll.a")
    set(MEDIASTUDIO_GLEW_LIBRARY "${MEDIASTUDIO_GLEW_ROOT}/lib/Release/x64/libglew32.dll.a")
elseif(EXISTS "${MEDIASTUDIO_GLEW_ROOT}/lib/Release/x64/glew32.lib")
    set(MEDIASTUDIO_GLEW_LIBRARY "${MEDIASTUDIO_GLEW_ROOT}/lib/Release/x64/glew32.lib")
elseif(EXISTS "${MEDIASTUDIO_GLEW_ROOT}/bin/Release/x64/glew32.dll")
    set(MEDIASTUDIO_GLEW_LIBRARY "${MEDIASTUDIO_GLEW_ROOT}/bin/Release/x64/glew32.dll")
else()
    message(FATAL_ERROR "GLEW x64 library not found under ${MEDIASTUDIO_GLEW_ROOT}")
endif()

mediastudio_import_unknown_library(GLEW::GLEW "${MEDIASTUDIO_GLEW_LIBRARY}")

target_link_libraries(MediaStudio PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    OpenGL::GL
    GLEW::GLEW
    ${MEDIASTUDIO_FFMPEG_TARGETS}
)

include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(MediaStudioPlaybackTests
        tests/PlaybackTests.cpp
        tests/RtspBlackhole.cpp
        tests/LiveProtocolServer.cpp
        ${MEDIASTUDIO_PLAYBACK_SOURCES}
    )
    set_target_properties(MediaStudioPlaybackTests PROPERTIES
        AUTOMOC OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests"
    )
    target_include_directories(MediaStudioPlaybackTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MEDIASTUDIO_FFMPEG_ROOT}/include"
    )
    target_link_libraries(MediaStudioPlaybackTests PRIVATE
        Threads::Threads
        Qt6::Core
        FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::avfilter
        FFmpeg::swresample FFmpeg::swscale
    )
    if(WIN32)
        target_compile_definitions(MediaStudioPlaybackTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(MediaStudioPlaybackTests PRIVATE ws2_32 iphlpapi)
    endif()

    # 测试可直接从构建目录运行，不依赖播放器部署步骤或已配置的全局 PATH。
    get_filename_component(mediastudio_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(test_case IN ITEMS contracts local-file rtsp-cancel rtsp-timeout rtsp-stable rtsp-presets rtsp-recovery-deadline
                          live-addresses live-options live-protocol-rtmp live-protocol-srt
                          live-cancel-rtmp live-cancel-srt live-timeout-rtmp live-timeout-srt live-private-failures)
        add_test(NAME MediaStudio.Playback.${test_case} COMMAND MediaStudioPlaybackTests ${test_case})
        set_tests_properties(MediaStudio.Playback.${test_case} PROPERTIES TIMEOUT 18)
        if(test_case STREQUAL "live-protocol-srt")
            set_property(TEST MediaStudio.Playback.${test_case} PROPERTY TIMEOUT 80)
        endif()
        if(test_case MATCHES "^live-")
            set_property(TEST MediaStudio.Playback.${test_case} PROPERTY FAIL_REGULAR_EXPRESSION "MediaStudioSecret[0-9A-Za-z]+")
        endif()
        if(WIN32)
            set_property(TEST MediaStudio.Playback.${test_case} PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${MEDIASTUDIO_FFMPEG_ROOT}/bin"
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                "PATH=path_list_prepend:${mediastudio_compiler_bin}"
            )
        endif()
    endforeach()
    # CTest 也检查进程输出，覆盖 FFmpeg 的 C 级 stderr 与 Qt 调试日志。
    set_property(TEST MediaStudio.Playback.rtsp-stable PROPERTY FAIL_REGULAR_EXPRESSION "MediaStudioSecret[0-9A-Za-z]+")

    # 队列容量、引用所有权与条件变量唤醒单独验证，不依赖 Qt 窗口或音频设备。
    add_executable(MediaStudioQueueTests tests/QueueTests.cpp src/PacketQueue.cpp)
    set_target_properties(MediaStudioQueueTests PROPERTIES AUTOMOC OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    target_include_directories(MediaStudioQueueTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MEDIASTUDIO_FFMPEG_ROOT}/include")
    target_link_libraries(MediaStudioQueueTests PRIVATE Threads::Threads FFmpeg::avcodec FFmpeg::avutil)
    foreach(test_case IN ITEMS limits timestamps ownership backpressure concurrency lifecycle)
        set(test_name MediaStudio.Queue.${test_case})
        add_test(NAME ${test_name} COMMAND MediaStudioQueueTests ${test_case})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 10)
        if(WIN32)
            set_property(TEST ${test_name} PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${MEDIASTUDIO_FFMPEG_ROOT}/bin"
                "PATH=path_list_prepend:${mediastudio_compiler_bin}")
        endif()
    endforeach()

    if(WIN32)
        # 真实 WASAPI 容量/PCM 与一次性故障注入；检查失败客户端释放后才创建回退客户端。
        add_executable(MediaStudioAudioTests tests/AudioOutputTests.cpp src/AudioOutput.cpp)
        set_target_properties(MediaStudioAudioTests PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
        target_include_directories(MediaStudioAudioTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(MediaStudioAudioTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(MediaStudioAudioTests PRIVATE ole32 uuid)
        foreach(test_case IN ITEMS default low fallback fallback-fails fallback-cancel invalid format-failure)
            set(test_name MediaStudio.Audio.${test_case})
            add_test(NAME ${test_name} COMMAND MediaStudioAudioTests ${test_case})
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${mediastudio_compiler_bin}")
        endforeach()
    endif()

    # 接收观测的秒窗、DTS 可信度和生命周期使用显式时间核验，无网络、声卡或真实等待。
    add_executable(MediaStudioReceiveTests tests/LiveReceiveMonitorTests.cpp src/LiveReceiveMonitor.cpp)
    set_target_properties(MediaStudioReceiveTests PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    target_include_directories(MediaStudioReceiveTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    foreach(test_case IN ITEMS windows progress invalid selection stale lifecycle)
        set(test_name MediaStudio.Receive.${test_case})
        add_test(NAME ${test_name} COMMAND MediaStudioReceiveTests ${test_case})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 10)
        if(WIN32)
            set_property(TEST ${test_name} PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${mediastudio_compiler_bin}")
        endif()
    endforeach()

    # 候选/撤销/持续时间与混合恢复预算，使用显式单调时间核对，不以真实睡眠代替边界检查。
    add_executable(MediaStudioBacklogTests tests/LiveBacklogTests.cpp src/LiveBacklog.cpp src/LiveReceiveMonitor.cpp src/LiveRecovery.cpp src/MediaIo.cpp)
    set_target_properties(MediaStudioBacklogTests PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    target_include_directories(MediaStudioBacklogTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MEDIASTUDIO_FFMPEG_ROOT}/include")
    target_link_libraries(MediaStudioBacklogTests PRIVATE Threads::Threads)
    foreach(test_case IN ITEMS gates thresholds sustained withdrawal cancellation monitor-stream budget)
        set(test_name MediaStudio.Backlog.${test_case})
        add_test(NAME ${test_name} COMMAND MediaStudioBacklogTests ${test_case})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 10)
        if(WIN32)
            set_property(TEST ${test_name} PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${mediastudio_compiler_bin}")
        endif()
    endforeach()

    # 恢复策略使用可控单调时钟，不等待真实的 45 秒。
    add_executable(MediaStudioRecoveryTests tests/RecoveryTests.cpp src/LiveRecovery.cpp src/MediaIo.cpp)
    set_target_properties(MediaStudioRecoveryTests PROPERTIES
        AUTOMOC OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests"
    )
    target_include_directories(MediaStudioRecoveryTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MEDIASTUDIO_FFMPEG_ROOT}/include"
    )
    target_link_libraries(MediaStudioRecoveryTests PRIVATE Threads::Threads)
    foreach(test_case IN ITEMS classification schedule deadline health cancellation watchdogs)
        set(test_name MediaStudio.Recovery.${test_case})
        add_test(NAME ${test_name} COMMAND MediaStudioRecoveryTests ${test_case})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 10)
        if(WIN32)
            set_property(TEST ${test_name} PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${mediastudio_compiler_bin}")
        endif()
    endforeach()

    # 会话和关窗用例链接实际主窗口、控制器及显示控件，离屏执行，无需数据库或音频设备。
    add_executable(MediaStudioSessionTests
        tests/ControllerTests.cpp
        tests/RtspBlackhole.cpp
        tests/LiveProtocolServer.cpp
        ${MEDIASTUDIO_PLAYBACK_SOURCES}
        ${MEDIASTUDIO_SESSION_SOURCES}
        src/MainWindow.cpp
        src/MediaTranscoder.cpp
    )
    set_target_properties(MediaStudioSessionTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests"
    )
    target_include_directories(MediaStudioSessionTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MEDIASTUDIO_FFMPEG_ROOT}/include"
        "${MEDIASTUDIO_GLEW_ROOT}/include"
    )
    target_link_libraries(MediaStudioSessionTests PRIVATE
        Threads::Threads
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets
        OpenGL::GL GLEW::GLEW
        FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::avfilter
        FFmpeg::swresample FFmpeg::swscale
    )
    target_compile_definitions(MediaStudioSessionTests PRIVATE MEDIASTUDIO_ENABLE_MYSQL=0)
    if(WIN32)
        target_compile_definitions(MediaStudioSessionTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(MediaStudioSessionTests PRIVATE ole32 uuid winmm ws2_32 iphlpapi)
    endif()

    foreach(test_case IN ITEMS controller-local controller-supersede controller-blocked-read packet-epochs widget-epochs
                          window-close-open window-close-probe window-close-read window-stop-reopen window-close-transcode
                          window-close-modal window-mixed-rtsp live-start-av-tcp live-start-av-udp
                          live-start-av-low-tcp live-start-av-low-udp
                          live-start-video-tcp live-start-video-udp live-start-audio-tcp live-start-audio-udp
                          live-start-no-key live-start-av-offset-tcp live-pause-open live-pause-probe live-pause-read
                          live-pause-av-tcp live-pause-av-udp live-command-races window-live-controls
                          live-cycle-video-tcp live-cycle-video-udp live-cycle-audio-tcp live-cycle-audio-udp
                          mixed-switch-tcp mixed-switch-udp live-failures-tcp live-failures-udp
                          live-recover-av-tcp live-recover-av-udp live-recover-video-tcp live-recover-video-udp
                          live-recover-audio-tcp live-recover-audio-udp live-recovery-budget live-recovery-cancel
                          live-stall-audio live-stall-video live-recovery-metadata window-auto-recovery
                          live-queue-audio-tcp live-queue-audio-udp live-queue-video-tcp live-queue-video-udp
                          live-queue-cancel live-oversized-video-tcp window-queue-failure
                          live-jitter-hold-tcp live-jitter-hold-udp live-jitter-gap-tcp live-jitter-gap-udp
                          live-frame-categories-tcp live-frame-categories-udp live-stall-audio-udp live-stall-video-tcp
                          live-fault-blackhole-open-tcp live-fault-blackhole-open-udp
                          live-fault-blackhole-probe-tcp live-fault-blackhole-probe-udp
                          live-fault-blackhole-read-tcp live-fault-blackhole-read-udp
                          live-fault-responses-tcp live-fault-responses-udp live-real-time-budget window-transient-buffering
                          live-buffer-presets window-audio-buffer-report live-receive-tcp live-receive-udp
                          live-receive-auto-reset window-receive-details
                          live-backlog-recover-tcp live-backlog-recover-udp live-backlog-transient live-backlog-invalid
                          live-backlog-stable live-backlog-disabled live-backlog-budget
                          live-backlog-cancel-candidate live-backlog-cancel-backoff window-low-latency
                          live-preset-jitter-stable-tcp live-preset-jitter-stable-udp
                          live-preset-jitter-low-tcp live-preset-jitter-low-udp live-backlog-budget-udp
                          live-protocol-rtmp-stable live-protocol-rtmp-low live-protocol-srt-stable live-protocol-srt-low
                          live-protocol-rtmp-stable-10fps live-protocol-rtmp-low-10fps
                          live-protocol-srt-stable-10fps live-protocol-srt-low-10fps
                          live-protocol-srt-low-late-video
                          live-protocol-cancel-open-rtmp live-protocol-cancel-open-srt
                          live-protocol-cancel-read-rtmp live-protocol-cancel-read-srt
                          live-protocol-cancel-backoff-rtmp live-protocol-cancel-backoff-srt
                          window-protocol-rtmp window-protocol-srt)
        set(test_name MediaStudio.Session.${test_case})
        add_test(NAME ${test_name} COMMAND MediaStudioSessionTests ${test_case})
        set_tests_properties(${test_name} PROPERTIES
            TIMEOUT 18
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:MediaStudioSessionTests>"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
        # 无默认音频端点的机器不能真实提交 PCM；其他错误保持测试失败。
        if(test_case MATCHES "^live-protocol-" OR test_case MATCHES "^window-protocol-")
            set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
                FAIL_REGULAR_EXPRESSION "MediaStudioSecret[0-9A-Za-z]+")
        endif()
        if(test_case MATCHES "^live-start-(av|audio)-" OR test_case MATCHES "^live-pause-av-" OR
           test_case MATCHES "^live-cycle-audio-" OR test_case MATCHES "^mixed-switch-" OR
           test_case MATCHES "^live-recover-(av|audio)-" OR test_case MATCHES "^live-stall-" OR
           test_case STREQUAL "live-recovery-metadata" OR test_case STREQUAL "live-buffer-presets" OR test_case MATCHES "^live-queue-" OR
           test_case STREQUAL "window-queue-failure" OR test_case MATCHES "^live-jitter-" OR
           test_case MATCHES "^live-frame-categories-")
            set_property(TEST ${test_name} PROPERTY SKIP_RETURN_CODE 77)
        endif()
        # 连续切换须逐个等待真实 PCM/视频输出，慢速测试机仍有有界的失败期限。
        if(test_case MATCHES "^live-receive-")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 30)
        endif()
        if(test_case MATCHES "^live-backlog-")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 35)
        endif()
        if(test_case STREQUAL "window-low-latency")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 35)
        endif()
        if(test_case MATCHES "^live-preset-jitter-")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 25)
        endif()
        if(test_case MATCHES "^live-backlog-budget")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 65)
        endif()
        if(test_case MATCHES "^mixed-switch-")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 45)
        endif()
        if(test_case MATCHES "^live-recover" OR test_case MATCHES "^live-stall-" OR test_case MATCHES "^live-queue-")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 35)
        endif()
        if(test_case MATCHES "^live-fault-blackhole-")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 45)
        endif()
        if(test_case STREQUAL "live-real-time-budget")
            set_property(TEST ${test_name} PROPERTY TIMEOUT 60)
        endif()
        if(test_case MATCHES "^live-fault-responses-")
            set_property(TEST ${test_name} PROPERTY FAIL_REGULAR_EXPRESSION "MediaStudioSecret[0-9A-Za-z]+")
        endif()
        if(TARGET Qt6::QOffscreenIntegrationPlugin)
            set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT
                "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
        endif()
        if(WIN32)
            set_property(TEST ${test_name} PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${MEDIASTUDIO_FFMPEG_ROOT}/bin"
                "PATH=path_list_prepend:${MEDIASTUDIO_GLEW_ROOT}/bin/Release/x64"
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                "PATH=path_list_prepend:${mediastudio_compiler_bin}"
            )
        endif()
    endforeach()
    set_property(TEST MediaStudio.Session.window-mixed-rtsp PROPERTY FAIL_REGULAR_EXPRESSION "MediaStudioSecret[0-9A-Za-z]+")
endif()

if(WIN32)
    target_link_libraries(MediaStudio PRIVATE ole32 uuid winmm)
endif()

if(MEDIASTUDIO_ENABLE_MYSQL)
    set(MEDIASTUDIO_MYSQL_INCLUDE_DIR "${MEDIASTUDIO_MYSQL_CONNECTOR_ROOT}/include/mysql")
    set(MEDIASTUDIO_MYSQL_LIBRARY "${MEDIASTUDIO_MYSQL_CONNECTOR_ROOT}/lib/libmariadb.dll.a")

    if(NOT EXISTS "${MEDIASTUDIO_MYSQL_INCLUDE_DIR}/mysql.h")
        message(FATAL_ERROR "MariaDB/MySQL C API header not found: ${MEDIASTUDIO_MYSQL_INCLUDE_DIR}/mysql.h")
    endif()

    target_include_directories(MediaStudio PRIVATE "${MEDIASTUDIO_MYSQL_INCLUDE_DIR}")
    mediastudio_import_unknown_library(MySQL::mariadb "${MEDIASTUDIO_MYSQL_LIBRARY}")
    target_link_libraries(MediaStudio PRIVATE MySQL::mariadb)
endif()

if(MEDIASTUDIO_DEPLOY_RUNTIME AND WIN32)
    file(GLOB MEDIASTUDIO_FFMPEG_RUNTIME_DLLS "${MEDIASTUDIO_FFMPEG_ROOT}/bin/*.dll")
    if(MEDIASTUDIO_FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET MediaStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MEDIASTUDIO_FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:MediaStudio>
        )
    endif()

    if(EXISTS "${MEDIASTUDIO_GLEW_ROOT}/bin/Release/x64/glew32.dll")
        add_custom_command(TARGET MediaStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MEDIASTUDIO_GLEW_ROOT}/bin/Release/x64/glew32.dll" $<TARGET_FILE_DIR:MediaStudio>
        )
    endif()

    if(MEDIASTUDIO_ENABLE_MYSQL)
        file(GLOB MEDIASTUDIO_MYSQL_RUNTIME_DLLS "${MEDIASTUDIO_MYSQL_CONNECTOR_ROOT}/bin/*.dll")
        list(FILTER MEDIASTUDIO_MYSQL_RUNTIME_DLLS EXCLUDE REGEX ".*(libgcc_s_seh-1|libstdc\\+\\+-6|libwinpthread-1)\\.dll$")
        if(MEDIASTUDIO_MYSQL_RUNTIME_DLLS)
            add_custom_command(TARGET MediaStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MEDIASTUDIO_MYSQL_RUNTIME_DLLS} $<TARGET_FILE_DIR:MediaStudio>
            )
        endif()
    endif()

    if(EXISTS "${MEDIASTUDIO_QT_ROOT}/bin/windeployqt.exe")
        add_custom_command(TARGET MediaStudio POST_BUILD
            COMMAND "${MEDIASTUDIO_QT_ROOT}/bin/windeployqt.exe" --no-translations $<TARGET_FILE:MediaStudio>
        )
    endif()
endif()
