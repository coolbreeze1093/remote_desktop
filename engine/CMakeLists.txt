# rdc — 纯 C++ WebRTC 引擎静态库（无 Qt）+ 协议单测。
# 依赖由根 CMakeLists 提供：datachannel（libdatachannel FetchContent 目标）、
# OPENH264_SOURCE_DIR（OpenH264 头）、LIBYUV_INC（libyuv 头，符号在
# screen_capture 静态库内，最终可执行文件链接时解析）。
cmake_minimum_required(VERSION 3.16)

set(RDC_SOURCES
  src/engine.cc
  src/peer_wrapper.cc
  src/media_sender.cc
  src/control_port.cc
  src/video_convert.cc
  src/video_codec.cc
)

add_library(rdc STATIC ${RDC_SOURCES})
target_include_directories(rdc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(rdc PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${LIBYUV_INC}
  ${OPENH264_SOURCE_DIR}/codec/api/wels)
# datachannel/openh264 随 rdc PUBLIC 传递给所有消费者。
target_link_libraries(rdc PUBLIC datachannel openh264)
target_link_libraries(rdc PRIVATE rdc_yuv)

# ---------------------------------------------------------------------------
# rdc_yuv — vendored libyuv（与 screen_capture_lib 相同的文件集与开关）。
# 单独成库使 rdc 自包含：宿主只链 rdc（如 rdc_cli_viewer）也能解析转换符号。
# GUI 同时链接 rdc 与 screen_capture 时两份目标文件符号集完全一致，
# 链接器只拉取先扫描到的那份，不会重复定义。
# ---------------------------------------------------------------------------
set(RDC_LIBYUV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/screen_capture_lib/vendored/third_party/libyuv")
file(GLOB RDC_LIBYUV_GENERIC "${RDC_LIBYUV_DIR}/source/*.cc")
list(FILTER RDC_LIBYUV_GENERIC EXCLUDE REGEX "_win\\.cc$")
list(FILTER RDC_LIBYUV_GENERIC EXCLUDE REGEX "_gcc\\.cc$")
if(MSVC)
  file(GLOB RDC_LIBYUV_ISA "${RDC_LIBYUV_DIR}/source/*_win.cc")
else()
  file(GLOB RDC_LIBYUV_ISA "${RDC_LIBYUV_DIR}/source/*_gcc.cc")
endif()

add_library(rdc_yuv STATIC ${RDC_LIBYUV_GENERIC} ${RDC_LIBYUV_ISA})
target_include_directories(rdc_yuv PUBLIC ${RDC_LIBYUV_DIR}/include)
target_compile_definitions(rdc_yuv PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX __STDC_FORMAT_MACROS
  DYNAMIC_ANNOTATIONS_ENABLED=0 ABSL_ALLOCATOR_NOTHROW=1
  LIBYUV_DISABLE_NEON LIBYUV_DISABLE_LSX LIBYUV_DISABLE_LASX NVALGRIND)
if(MINGW)
  # 与 SCL 一致：MinGW 下禁用 x86 SIMD（GCC 版本的 AVX2 汇编在运行时崩溃），
  # 标量路径每帧多几毫秒，相对编码可忽略。
  target_compile_definitions(rdc_yuv PRIVATE LIBYUV_DISABLE_X86)
  target_compile_options(rdc_yuv PRIVATE -fno-tree-vectorize -fno-tree-slp-vectorize)
endif()

# 控制协议往返单测（纯函数，无网络依赖）。
add_executable(rdc_protocol_test tests/protocol_test.cc)
target_include_directories(rdc_protocol_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rdc_protocol_test PRIVATE rdc)
