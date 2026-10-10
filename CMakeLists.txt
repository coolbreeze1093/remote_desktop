
cmake_minimum_required(VERSION 3.21)

project(rtcvpn LANGUAGES C CXX)

# ============================================================
# C++ 标准
# ============================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# 编译器选项
# ============================================================

if(MSVC)
    message(STATUS "Compiler: MSVC")
    add_compile_options(/W4 /utf-8)
    set(MY_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Windows")

elseif(MINGW)
    message(STATUS "Compiler: MinGW")
    add_compile_options(-Wall)
    set(MY_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Windows")

elseif(UNIX)
    message(STATUS "Compiler: Unix")
    add_compile_options(-Wall)
    set(MY_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Linux")

else()
    message(FATAL_ERROR "Unsupported compiler/platform")
endif()

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
        -Wno-error=format
        -Wno-error=format-extra-args
    )
endif()

# ============================================================
# 目录
# ============================================================

set(DOWNLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/downloads")
set(BUILD_DIR "${CMAKE_BINARY_DIR}/builds")

file(MAKE_DIRECTORY "${DOWNLOAD_DIR}")
file(MAKE_DIRECTORY "${BUILD_DIR}")

include(FetchContent)

# ============================================================
# 统一设置第三方库选项
# 必须放在 FetchContent_MakeAvailable 之前
# ============================================================

# libdatachannel
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)

set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(USE_OPENSSL ON CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

# OpenH264
FetchContent_Declare(
    openh264
    GIT_REPOSITORY https://github.com/cisco/openh264.git
    GIT_TAG v2.6.0
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(openh264)
if(NOT openh264_POPULATED)
    FetchContent_Populate(openh264)
endif()

find_program(GNU_MAKE_EXECUTABLE NAMES mingw32-make.exe make.exe make)
if(NOT GNU_MAKE_EXECUTABLE)
    message(FATAL_ERROR "未找到 MinGW Make，请检查 MinGW 的 bin 目录是否加入 PATH")
endif()

set(OPENH264_SOURCE_DIR "${openh264_SOURCE_DIR}")

add_custom_target(openh264_build
    COMMAND "${GNU_MAKE_EXECUTABLE}" OS=mingw_nt
    WORKING_DIRECTORY "${OPENH264_SOURCE_DIR}"
    COMMENT "使用 MinGW Make 构建 OpenH264"
    VERBATIM
)


FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.5
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(libdatachannel)

message(STATUS "All third-party dependencies configured.")

add_subdirectory(third_party/screen_capture_lib)

add_subdirectory(src)
