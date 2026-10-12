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
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# --- Sources ----------------------------------------------------------------
# WebRTC 相关能力全部在 rdc 引擎静态库中（../engine），GUI 目标只链接 rdc。
# 旧的 net/peer_session、net/control_channel、net/h264_stream_sender、
# core/input_event、core/openh264_codec（引擎内已有副本）已从目标中移除。
add_executable(remote_desktop WIN32
  main.cpp
  core/input_desktop.cc
  core/input_injector.cc
  session/remote_desktop_session.cc
  ui/main_window.cc
  ui/remote_video_window.cc
  ui/video_widget.cc)

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
  ui/main_window.cc
  ui/remote_video_window.cc
  ui/video_widget.cc)

target_include_directories(remote_desktop PRIVATE
  ./)

target_include_directories(rd_self_test PRIVATE
  ./)

target_include_directories(rd_pair_test PRIVATE
  ./)

target_include_directories(rd_mini_test PRIVATE
  ./)

target_link_libraries(rd_mini_test PRIVATE
  Qt5::Widgets
  rdc
  screen_capture
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt
  shell32)

target_link_libraries(remote_desktop PRIVATE
  Qt5::Widgets
  rdc
  screen_capture
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
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

if(WIN32)
  target_link_libraries(remote_desktop PRIVATE
    user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)
  if(MINGW)
    target_link_libraries(remote_desktop PRIVATE stdc++fs)
  endif()
endif()

# Deploy Qt DLLs next to the exe after build.
if(WIN32 AND MINGW)
  find_program(SCL_WINDEPLOYQT_EXE windeployqt HINTS "${Qt5_DIR}/../../bin")
  add_custom_command(TARGET remote_desktop POST_BUILD
    COMMAND "${SCL_WINDEPLOYQT_EXE}" --no-translations --no-opengl-sw "$<TARGET_FILE:remote_desktop>"
    COMMENT "Running windeployqt (release Qt DLLs)"
    )

  # OpenSSL 运行时 DLL 随 exe 部署（libcrypto/libssl，列表由根 CMakeLists 的
  # OPENSSL_RUNTIME_DLLS 提供）：目标机器没有 msys2/openssl 环境也能直接跑。
  # build/src 目录级生效，同目录的 rd_self_test 等测试 exe 一并覆盖。
  foreach(_ssl_dll ${OPENSSL_RUNTIME_DLLS})
    add_custom_command(TARGET remote_desktop POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_ssl_dll}" "$<TARGET_FILE_DIR:remote_desktop>"
      COMMENT "Deploying OpenSSL runtime DLL: ${_ssl_dll}"
      )
  endforeach()
  unset(_ssl_dll)
endif()
