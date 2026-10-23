# rd_host_service — 被控端服务 + 会话 helper（无 Qt）。
# 一个 exe 多模式：--service（SCM）/ --helper（会话内被控端）/ --install / --uninstall。
add_executable(rd_host_service
  service_main.cc
  host_helper.cc
  "${CMAKE_SOURCE_DIR}/src/core/input_desktop.cc"
  "${CMAKE_SOURCE_DIR}/src/core/input_injector.cc")

target_include_directories(rd_host_service PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/third_party/screen_capture_lib/include")

target_link_libraries(rd_host_service PRIVATE
  rdc
  screen_capture
  user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt
  wtsapi32)

if(MINGW)
  target_link_libraries(rd_host_service PRIVATE stdc++fs)
endif()

# OpenSSL 运行时 DLL（libdatachannel 动态依赖）与 GUI 部署策略一致。
foreach(_ssl_dll ${OPENSSL_RUNTIME_DLLS})
  add_custom_command(TARGET rd_host_service POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_ssl_dll}"
            "$<TARGET_FILE_DIR:rd_host_service>"
    COMMENT "Deploying OpenSSL runtime DLL: ${_ssl_dll}")
endforeach()

# 开发树对齐部署布局：GUI 的「安装被控服务」按钮只在自己目录找
# rd_host_service.exe（发布目录同目录，见 HostWindow::onSvcInstall /
# DirectHostWindow::onSvcInstall）。
# 默认生成器会把两者放 build/src 与 build/service，按钮必然找不到 exe。
# 依赖只允许 GUI -> rd_host_service 单向（反向会在生成期成环）：
# 构建完服务后尽力刷一份到 GUI 的输出目录（路径为当前默认布局）。
add_dependencies(rd_host rd_host_service)
add_dependencies(rd_direct_host rd_host_service)
add_custom_command(TARGET rd_host_service POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:rd_host_service>" "${CMAKE_BINARY_DIR}/src/"
  COMMENT "Deploying rd_host_service.exe next to rd_host.exe")
