
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

# 自建 OpenH264 静态库：仅当产物缺失（首次或新 build 目录）时才执行 make，
# 已有 ${OPENH264_SOURCE_DIR}/libopenh264.a 则直接复用（stamp 存在即跳过），
# 避免每次构建都触发 make（其版本生成步骤需要 msys2 的 POSIX shell）。
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/openh264-build.stamp"
    COMMAND "${GNU_MAKE_EXECUTABLE}" OS=mingw_nt
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/openh264-build.stamp"
    WORKING_DIRECTORY "${OPENH264_SOURCE_DIR}"
    BYPRODUCTS "${OPENH264_SOURCE_DIR}/libopenh264.a"
    COMMENT "使用 MinGW Make 构建 OpenH264（静态库 libopenh264.a）"
    VERBATIM
)
add_custom_target(openh264_build
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/openh264-build.stamp"
)

# 链接上面自建的静态库 ${OPENH264_SOURCE_DIR}/libopenh264.a：
# 用完整路径显式指定，避免裸 -lopenh264 落到编译器默认搜索路径
# （msys2 的旧版 openh264，且 .dll.a 先命中会变成动态链接 libopenh264-7.dll）。
add_library(openh264 STATIC IMPORTED GLOBAL)
set_target_properties(openh264 PROPERTIES
    IMPORTED_LOCATION "${OPENH264_SOURCE_DIR}/libopenh264.a"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENH264_SOURCE_DIR}/codec/api/wels"
)

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.5
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(libdatachannel)

# OpenSSL 运行时 DLL：libdatachannel 动态链接 OpenSSL（USE_OPENSSL=ON），
# exe 依赖 libcrypto-3-x64.dll / libssl-3-x64.dll（位于编译器默认搜索路径，
# 如 msys2 的 mingw64/bin，目标机器没有该环境就起不来）。这两个 DLL 只依赖
# 系统 DLL（CRYPT32/WS2_32 等），运行时闭包就是这两个文件；由 src 的
# POST_BUILD 拷到 exe 旁，发布目录可直接使用。
set(OPENSSL_RUNTIME_DLLS "")
# 从缓存的 OPENSSL_INCLUDE_DIR（libdatachannel 内 find_package(OpenSSL) 写入）
# 推出 OpenSSL 根目录，再在 bin 下找运行时 DLL；libdatachannel 子目录里的
# find_package 变量/目标在根作用域不可见，故不依赖它们。
if(OPENSSL_INCLUDE_DIR)
    get_filename_component(_OPENSSL_ROOT "${OPENSSL_INCLUDE_DIR}" DIRECTORY)  # .../mingw64
    file(GLOB OPENSSL_RUNTIME_DLLS
        "${_OPENSSL_ROOT}/bin/libcrypto-*.dll"
        "${_OPENSSL_ROOT}/bin/libssl-*.dll")
    list(LENGTH OPENSSL_RUNTIME_DLLS _OPENSSL_DLL_COUNT)
    if(_OPENSSL_DLL_COUNT GREATER 0)
        message(STATUS "OpenSSL runtime DLLs (${_OPENSSL_DLL_COUNT}): ${OPENSSL_RUNTIME_DLLS}")
    else()
        set(OPENSSL_RUNTIME_DLLS "")
        message(WARNING "在 ${_OPENSSL_ROOT}/bin 下未找到 OpenSSL 运行时 DLL，跳过部署")
    endif()
    unset(_OPENSSL_ROOT)
    unset(_OPENSSL_DLL_COUNT)
else()
    message(WARNING "未定位到 OpenSSL（OPENSSL_INCLUDE_DIR 为空），跳过运行时 DLL 部署")
endif()

message(STATUS "All third-party dependencies configured.")

add_subdirectory(third_party/screen_capture_lib)

# libyuv 头文件（符号在 screen_capture 静态库内，最终链接时解析）；
# rdc 引擎与 src 共用。
set(LIBYUV_INC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/screen_capture_lib/vendored/third_party/libyuv/include")

# rdc — 纯 C++ WebRTC 引擎（无 Qt），供 GUI 与 CLI 共用。
add_subdirectory(engine)
# 引擎链接的是自建 openh264 静态库：先跑 openh264_build，再编 rdc 与最终链接。
add_dependencies(rdc openh264_build)

# 无 Qt CLI demo（rdc 引擎独立使用样例）。
add_subdirectory(examples)

add_subdirectory(src)
