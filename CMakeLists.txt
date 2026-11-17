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
    src/MediaDecoder.cpp
)

set(MEDIASTUDIO_SESSION_SOURCES
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
    src/MediaIo.h
    src/MediaInfo.h
    src/MediaDecoder.h
    src/AudioOutput.h
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
        target_link_libraries(MediaStudioPlaybackTests PRIVATE ws2_32)
    endif()

    # 测试可直接从构建目录运行，不依赖播放器部署步骤或已配置的全局 PATH。
    get_filename_component(mediastudio_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(test_case IN ITEMS contracts local-file rtsp-cancel rtsp-timeout rtsp-stable)
        add_test(NAME MediaStudio.Playback.${test_case} COMMAND MediaStudioPlaybackTests ${test_case})
        set_tests_properties(MediaStudio.Playback.${test_case} PROPERTIES TIMEOUT 18)
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

    # 会话和关窗用例链接实际主窗口、控制器及显示控件，离屏执行，无需数据库或音频设备。
    add_executable(MediaStudioSessionTests
        tests/ControllerTests.cpp
        tests/RtspBlackhole.cpp
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
        target_link_libraries(MediaStudioSessionTests PRIVATE ole32 uuid winmm ws2_32)
    endif()

    foreach(test_case IN ITEMS controller-local controller-supersede controller-blocked-read packet-epochs widget-epochs
                          window-close-open window-close-probe window-close-read window-stop-reopen window-close-transcode
                          window-close-modal window-mixed-rtsp live-start-av-tcp live-start-av-udp
                          live-start-video-tcp live-start-video-udp live-start-audio-tcp live-start-audio-udp
                          live-start-no-key live-start-av-offset-tcp)
        set(test_name MediaStudio.Session.${test_case})
        add_test(NAME ${test_name} COMMAND MediaStudioSessionTests ${test_case})
        set_tests_properties(${test_name} PROPERTIES
            TIMEOUT 18
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:MediaStudioSessionTests>"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
        # 无默认音频端点的机器不能真实提交 PCM；其他错误保持测试失败。
        if(test_case MATCHES "^live-start-(av|audio)-")
            set_property(TEST ${test_name} PROPERTY SKIP_RETURN_CODE 77)
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
