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

include_directories(${OPENH264_SOURCE_DIR}/codec/api/wels)

# --- libyuv (from screen_capture_lib vendored tree; header-only for us,
#     the symbols live in the screen_capture static lib already) -----------
set(LIBYUV_INC "${SCL_DIR}/vendored/third_party/libyuv/include")
message(STATUS "LIBYUV_INC: ${LIBYUV_INC}")

# --- Sources ----------------------------------------------------------------
add_executable(remote_desktop WIN32
  main.cpp
  core/i420_frame.cc
  core/openh264_codec.cc
  core/input_event.cc
  core/input_injector.cc
  net/peer_session.cc
  net/control_channel.cc
  net/h264_stream_sender.cc
  session/remote_desktop_session.cc
  ui/main_window.cc
  ui/remote_video_window.cc
  ui/video_widget.cc)

# Headless end-to-end self test (console app, same libraries).
add_executable(rd_self_test
  self_test.cc
  core/i420_frame.cc
  core/openh264_codec.cc
  core/input_event.cc
  core/input_injector.cc
  net/peer_session.cc
  net/control_channel.cc
  net/h264_stream_sender.cc
  session/remote_desktop_session.cc)

# Two-process pair test: SDP exchanged via files in the CWD.
add_executable(rd_pair_test
  pair_test.cc
  core/i420_frame.cc
  core/openh264_codec.cc
  core/input_event.cc
  core/input_injector.cc
  net/peer_session.cc
  net/control_channel.cc
  net/h264_stream_sender.cc
  session/remote_desktop_session.cc)

# GUI repro harness: full host+viewer pipeline in one process plus a real
# RemoteVideoWindow; scripted minimize/restore to chase the "minimize freezes
# the remote" report.
add_executable(rd_mini_test
  mini_test.cc
  core/i420_frame.cc
  core/openh264_codec.cc
  core/input_event.cc
  core/input_injector.cc
  net/peer_session.cc
  net/control_channel.cc
  net/h264_stream_sender.cc
  session/remote_desktop_session.cc
  ui/main_window.cc
  ui/remote_video_window.cc
  ui/video_widget.cc)

target_include_directories(remote_desktop PRIVATE
  ./
  ${LIBYUV_INC})

target_include_directories(rd_self_test PRIVATE
  ./
  ${LIBYUV_INC})

target_include_directories(rd_pair_test PRIVATE
  ./
  ${LIBYUV_INC})

target_include_directories(rd_mini_test PRIVATE
  ./
  ${LIBYUV_INC})

target_link_libraries(rd_mini_test PRIVATE
  Qt5::Widgets
  screen_capture
  datachannel
  openh264
  
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)

target_link_libraries(remote_desktop PRIVATE
  Qt5::Widgets
  screen_capture
  datachannel
  openh264
  
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB})

add_executable(rd_enc_test repro_enc.cc
  core/openh264_codec.cc
  core/i420_frame.cc)

target_include_directories(rd_enc_test PRIVATE
  ./
  ${LIBYUV_INC}
  ${OPENH264_DIR}/codec/api/wels)

target_link_libraries(rd_enc_test PRIVATE
  screen_capture
  openh264
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi)

target_link_libraries(rd_self_test PRIVATE
  Qt5::Core Qt5::Gui
  screen_capture
  datachannel
  openh264
  
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)

target_link_libraries(rd_pair_test PRIVATE
  Qt5::Core Qt5::Gui
  screen_capture
  datachannel
  openh264
  
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
endif()
