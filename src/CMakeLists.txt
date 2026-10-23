cmake_minimum_required(VERSION 3.16)
project(remote_desktop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Paths (adjust via -D if your layout differs) --------------------------
set(SCL_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/screen_capture_lib")
message(STATUS "SCL_DIR: ${SCL_DIR}")

# --- Qt --------------------------------------------------------------------
set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};F:/Qt/Qt5.12.12/5.12.12/mingw73_64")
find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

# --- Sources ----------------------------------------------------------------
# WebRTC 相关能力全部在 rdc 引擎静态库中（../engine），GUI 目标只链接 rdc。
# 旧的 net/peer_session、net/control_channel、net/h264_stream_sender、
# core/input_event、core/openh264_codec（引擎内已有副本）已从目标中移除。
#
# 客户端与服务端拆成两个程序：
#   rd_host   服务端（被控端）— 本机推流 + 被控服务面板（rd_host_service）
#   rd_viewer 客户端（主控端）— Offer/Answer 建连 + 远端画面与键鼠转发
# 公共的会话适配（session/）与窗口基类（ui/session_window）两边各编一份。

# 服务端：本机推流 + 被控服务面板（pipe_proto.h 在 ../service）。
add_executable(rd_host WIN32
  host_main.cpp
  core/input_desktop.cc
  core/input_injector.cc
  service/host_service_client.cc
  session/remote_desktop_session.cc
  ui/session_window.cc
  ui/host_window.cc)

# 客户端：远端画面窗口 + 输入转发（不需要被控服务/ShellExecute）。
add_executable(rd_viewer WIN32
  viewer_main.cpp
  core/input_desktop.cc
  core/input_injector.cc
  session/remote_desktop_session.cc
  ui/session_window.cc
  ui/viewer_window.cc
  ui/remote_video_window.cc
  ui/video_widget.cc)

# 直连版：被控端监听端口 + 主控端 IP 直连。TCP 信令只自动交换 SDP，
# 媒体与控制仍走 DataChannel P2P（不需要信令服务器，无手动 SDP 交换）。
# 被控服务面板与 rd_host 同款：装好 rd_host_service 后推流走 SYSTEM 服务。
add_executable(rd_direct_host WIN32
  direct_host_main.cpp
  core/input_desktop.cc
  core/input_injector.cc
  service/host_service_client.cc
  session/remote_desktop_session.cc
  session/direct_signaling.cc
  ui/session_window.cc
  ui/direct_host_window.cc)

add_executable(rd_direct_viewer WIN32
  direct_viewer_main.cpp
  core/input_desktop.cc
  core/input_injector.cc
  session/remote_desktop_session.cc
  session/direct_signaling.cc
  ui/session_window.cc
  ui/direct_viewer_window.cc
  ui/remote_video_window.cc
  ui/video_widget.cc)

# 直连信令层无头自测（控制台）。
add_executable(rd_direct_signaling_test
  direct_signaling_test.cc
  session/direct_signaling.cc)

# Headless end-to-end self test (console app, same libraries).
add_executable(rd_self_test
  self_test.cc
  core/input_desktop.cc
  core/input_injector.cc
  session/remote_desktop_session.cc)

# Two-process pair test: SDP exchanged via files in the CWD.
add_executable(rd_pair_test
  pair_test.cc
  core/input_desktop.cc
  core/input_injector.cc
  session/remote_desktop_session.cc)

# GUI repro harness: full host+viewer pipeline in one process plus a real
# RemoteVideoWindow; scripted minimize/restore to chase the "minimize freezes
# the remote" report.
add_executable(rd_mini_test
  mini_test.cc
  core/input_desktop.cc
  core/input_injector.cc
  session/remote_desktop_session.cc
  ui/remote_video_window.cc
  ui/video_widget.cc)

target_include_directories(rd_host PRIVATE
  ./
  "${CMAKE_CURRENT_SOURCE_DIR}/../service")

target_include_directories(rd_direct_host PRIVATE
  ./
  "${CMAKE_CURRENT_SOURCE_DIR}/../service")

target_include_directories(rd_direct_viewer PRIVATE
  ./)

target_include_directories(rd_direct_signaling_test PRIVATE
  ./)

target_include_directories(rd_viewer PRIVATE
  ./)

target_include_directories(rd_self_test PRIVATE
  ./)

target_include_directories(rd_pair_test PRIVATE
  ./)

target_include_directories(rd_mini_test PRIVATE
  ./)

# GUI 程序共用的链接集（rdc 引擎 + 采集库 + libdatachannel 依赖 + 系统库）。
set(RD_GUI_LIBS
  Qt5::Widgets
  rdc
  screen_capture
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)

target_link_libraries(rd_host PRIVATE ${RD_GUI_LIBS} shell32)
target_link_libraries(rd_viewer PRIVATE ${RD_GUI_LIBS})

target_link_libraries(rd_direct_host PRIVATE ${RD_GUI_LIBS} Qt5::Network shell32)
target_link_libraries(rd_direct_viewer PRIVATE ${RD_GUI_LIBS} Qt5::Network shell32)
target_link_libraries(rd_direct_signaling_test PRIVATE
  Qt5::Core Qt5::Network ws2_32)

target_link_libraries(rd_mini_test PRIVATE
  Qt5::Widgets
  rdc
  screen_capture
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt
  shell32)

# 编码器复现工具：链接 rdc，使用引擎的编解码/转换封装
# （src/core 里的重复实现已删除，日志也统一走 rdc/log.h）。
add_executable(rd_enc_test repro_enc.cc)

target_include_directories(rd_enc_test PRIVATE
  ./
  ${CMAKE_SOURCE_DIR}/engine/src)

target_link_libraries(rd_enc_test PRIVATE
  rdc
  screen_capture
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi)

target_link_libraries(rd_self_test PRIVATE
  Qt5::Core Qt5::Gui
  rdc
  screen_capture
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)

target_link_libraries(rd_pair_test PRIVATE
  Qt5::Core Qt5::Gui
  rdc
  screen_capture
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)

if(MINGW)
  target_link_libraries(rd_host PRIVATE stdc++fs)
  target_link_libraries(rd_viewer PRIVATE stdc++fs)
  target_link_libraries(rd_direct_host PRIVATE stdc++fs)
  target_link_libraries(rd_direct_viewer PRIVATE stdc++fs)
endif()

# Deploy Qt DLLs next to the exes after build (both GUI programs live in the
# same build dir; windeployqt is idempotent so running it per target is fine).
if(WIN32 AND MINGW)
  find_program(SCL_WINDEPLOYQT_EXE windeployqt HINTS "${Qt5_DIR}/../../bin")
  foreach(_gui_target rd_host rd_viewer rd_direct_host rd_direct_viewer)
    add_custom_command(TARGET ${_gui_target} POST_BUILD
      COMMAND "${SCL_WINDEPLOYQT_EXE}" --no-translations --no-opengl-sw "$<TARGET_FILE:${_gui_target}>"
      COMMENT "Running windeployqt (release Qt DLLs) for ${_gui_target}"
      )

    # OpenSSL 运行时 DLL 随 exe 部署（libcrypto/libssl，列表由根 CMakeLists 的
    # OPENSSL_RUNTIME_DLLS 提供）：目标机器没有 msys2/openssl 环境也能直接跑。
    foreach(_ssl_dll ${OPENSSL_RUNTIME_DLLS})
      add_custom_command(TARGET ${_gui_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_ssl_dll}" "$<TARGET_FILE_DIR:${_gui_target}>"
        COMMENT "Deploying OpenSSL runtime DLL: ${_ssl_dll}"
        )
    endforeach()
  endforeach()
  unset(_gui_target)
  unset(_ssl_dll)
endif()
