# rdc_cli_host / rdc_cli_viewer — 无 Qt 引擎使用样例（P3 验收项）。
# SDP 通过 CWD 下的 rdc_offer.txt / rdc_answer.txt 交换（同 rd_pair_test 约定）。
add_executable(rdc_cli_host rdc_cli_host.cc "${CMAKE_SOURCE_DIR}/src/core/input_desktop.cc" "${CMAKE_SOURCE_DIR}/src/core/input_injector.cc")
target_include_directories(rdc_cli_host PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/third_party/screen_capture_lib/include")
target_link_libraries(rdc_cli_host PRIVATE rdc screen_capture)
if(WIN32)
  target_link_libraries(rdc_cli_host PRIVATE
    user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi dwmapi iphlpapi bcrypt)
endif()

add_executable(rdc_cli_viewer rdc_cli_viewer.cc)
target_link_libraries(rdc_cli_viewer PRIVATE rdc)
