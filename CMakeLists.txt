cmake_minimum_required(VERSION 3.16)
project(remote_desktop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Paths (adjust via -D if your layout differs) --------------------------
set(SCL_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/../screen_capture_lib" CACHE PATH "")
set(LDC_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel" CACHE PATH "")
set(LDC_BUILD     "${LDC_DIR}/build" CACHE PATH "")
set(OPENH264_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openh264" CACHE PATH "")
set(MBEDTLS_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/install/mbedtls" CACHE PATH "")

# --- Qt --------------------------------------------------------------------
set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};F:/Qt/Qt5.12.12/5.12.12/mingw73_64")
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# --- screen_capture_lib (static, already built) ----------------------------
add_library(screen_capture STATIC IMPORTED)
set_target_properties(screen_capture PROPERTIES
  IMPORTED_LOCATION "${SCL_DIR}/build-qtmingw/libscreen_capture.a")
target_include_directories(screen_capture INTERFACE "${SCL_DIR}/include")

# --- libdatachannel (static, already built) --------------------------------
add_library(datachannel STATIC IMPORTED)
set_target_properties(datachannel PROPERTIES
  IMPORTED_LOCATION "${LDC_BUILD}/libdatachannel-static.a")
target_include_directories(datachannel INTERFACE "${LDC_DIR}/include")
target_compile_definitions(datachannel INTERFACE RTC_STATIC)

# libdatachannel deps (also static, built inside its build tree)
find_library(JUICE_LIB NAMES libjuice-static.a juice-static
  PATHS "${LDC_BUILD}/deps/libjuice" NO_DEFAULT_PATH)
find_library(SRTP_LIB NAMES libsrtp2.a srtp2
  PATHS "${LDC_BUILD}/deps/libsrtp" NO_DEFAULT_PATH)
find_library(USRSCTP_LIB NAMES libusrsctp.a usrsctp
  PATHS "${LDC_BUILD}/deps/usrsctp/usrsctplib" NO_DEFAULT_PATH)

# mbedtls
add_library(mbedtls STATIC IMPORTED)
add_library(mbedx509 STATIC IMPORTED)
add_library(mbedcrypto STATIC IMPORTED)
set_target_properties(mbedtls PROPERTIES IMPORTED_LOCATION "${MBEDTLS_ROOT}/lib/libmbedtls.a")
set_target_properties(mbedx509 PROPERTIES IMPORTED_LOCATION "${MBEDTLS_ROOT}/lib/libmbedx509.a")
set_target_properties(mbedcrypto PROPERTIES IMPORTED_LOCATION "${MBEDTLS_ROOT}/lib/libmbedcrypto.a")
file(MAKE_DIRECTORY "${MBEDTLS_ROOT}/include")
target_include_directories(mbedtls INTERFACE "${MBEDTLS_ROOT}/include")

# --- OpenH264 ---------------------------------------------------------------
add_library(openh264 STATIC IMPORTED)
set_target_properties(openh264 PROPERTIES
  IMPORTED_LOCATION "${OPENH264_DIR}/libopenh264.a")
target_include_directories(openh264 INTERFACE "${OPENH264_DIR}/codec/api/wels")

# --- libyuv (from screen_capture_lib vendored tree; header-only for us,
#     the symbols live in the screen_capture static lib already) -----------
set(LIBYUV_INC "${SCL_DIR}/vendored/third_party/libyuv/include")

# --- Sources ----------------------------------------------------------------
add_executable(remote_desktop WIN32
  src/main.cpp
  src/core/i420_frame.cc
  src/core/openh264_codec.cc
  src/core/input_event.cc
  src/core/input_injector.cc
  src/net/peer_session.cc
  src/net/control_channel.cc
  src/net/h264_stream_sender.cc
  src/session/remote_desktop_session.cc
  src/ui/main_window.cc
  src/ui/remote_video_window.cc
  src/ui/video_widget.cc)

# Headless end-to-end self test (console app, same libraries).
add_executable(rd_self_test
  src/self_test.cc
  src/core/i420_frame.cc
  src/core/openh264_codec.cc
  src/core/input_event.cc
  src/core/input_injector.cc
  src/net/peer_session.cc
  src/net/control_channel.cc
  src/net/h264_stream_sender.cc
  src/session/remote_desktop_session.cc)

# Two-process pair test: SDP exchanged via files in the CWD.
add_executable(rd_pair_test
  src/pair_test.cc
  src/core/i420_frame.cc
  src/core/openh264_codec.cc
  src/core/input_event.cc
  src/core/input_injector.cc
  src/net/peer_session.cc
  src/net/control_channel.cc
  src/net/h264_stream_sender.cc
  src/session/remote_desktop_session.cc)

# GUI repro harness: full host+viewer pipeline in one process plus a real
# RemoteVideoWindow; scripted minimize/restore to chase the "minimize freezes
# the remote" report.
add_executable(rd_mini_test
  src/mini_test.cc
  src/core/i420_frame.cc
  src/core/openh264_codec.cc
  src/core/input_event.cc
  src/core/input_injector.cc
  src/net/peer_session.cc
  src/net/control_channel.cc
  src/net/h264_stream_sender.cc
  src/session/remote_desktop_session.cc
  src/ui/main_window.cc
  src/ui/remote_video_window.cc
  src/ui/video_widget.cc)

target_include_directories(remote_desktop PRIVATE
  src
  ${LIBYUV_INC})

target_include_directories(rd_self_test PRIVATE
  src
  ${LIBYUV_INC})

target_include_directories(rd_pair_test PRIVATE
  src
  ${LIBYUV_INC})

target_include_directories(rd_mini_test PRIVATE
  src
  ${LIBYUV_INC})

target_link_libraries(rd_mini_test PRIVATE
  Qt5::Widgets
  screen_capture
  datachannel
  openh264
  mbedtls mbedx509 mbedcrypto
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)

target_link_libraries(remote_desktop PRIVATE
  Qt5::Widgets
  screen_capture
  datachannel
  openh264
  mbedtls mbedx509 mbedcrypto
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB})

add_executable(rd_enc_test src/repro_enc.cc
  src/core/openh264_codec.cc
  src/core/i420_frame.cc)

target_include_directories(rd_enc_test PRIVATE
  src
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
  mbedtls mbedx509 mbedcrypto
  ${JUICE_LIB} ${SRTP_LIB} ${USRSCTP_LIB}
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)

target_link_libraries(rd_pair_test PRIVATE
  Qt5::Core Qt5::Gui
  screen_capture
  datachannel
  openh264
  mbedtls mbedx509 mbedcrypto
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
    COMMAND "${SCL_WINDEPLOYQT_EXE}" --release --no-translations --no-opengl-sw "$<TARGET_FILE:remote_desktop>"
    COMMENT "Running windeployqt (release Qt DLLs)")
endif()
