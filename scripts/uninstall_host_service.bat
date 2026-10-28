@echo off
rem uninstall_host_service.bat - stop and uninstall the RdHostService service.
rem Made for double-click: elevates itself via UAC. Optionally deletes rd_host.ini.
setlocal EnableExtensions EnableDelayedExpansion
rem 本文件必须以 GBK(936) 编码保存：中文 Windows 的 cmd 默认按 936 解析，
rem 双击运行中文显示/解析都正确；不要转存为 UTF-8 或带 BOM。

rem ---- 自提权 ----
net session >nul 2>&1
if errorlevel 1 (
  echo 需要管理员权限，正在弹出 UAC 确认窗口...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
title RemoteDesktop 被控服务卸载

rem ---- 定位 exe（找不到也能用 sc 直接卸，仅防火墙规则需补删）----
set "EXE="
if exist "%~dp0rd_host_service.exe" set "EXE=%~dp0rd_host_service.exe"
if not defined EXE if exist "%~dp0..\publish\rd_host_service.exe" set "EXE=%~dp0..\publish\rd_host_service.exe"

set "INI="
if defined EXE for %%F in ("%EXE%") do set "INI=%%~dpFrd_host.ini"
if not defined INI set "INI=%~dp0rd_host.ini"

echo 正在停止并卸载被控服务 RdHostService...
if defined EXE (
  "%EXE%" --uninstall --quiet
  if errorlevel 1 (
    echo [错误] rd_host_service --uninstall 失败（退出码 %errorlevel%）。
    pause
    exit /b 1
  )
) else (
  rem exe 不在：直接走 SCM 卸载，防火墙规则手动补删。
  sc stop RdHostService >nul 2>&1
  timeout /t 2 /nobreak >nul
  sc delete RdHostService >nul 2>&1
  netsh advfirewall firewall delete rule name=RdHostServiceSignal >nul 2>&1
)
echo [完成] --uninstall 已执行。

rem ---- 可选：删除配置文件 ----
set "DEL="
if exist "%INI%" set /p DEL=同时删除配置文件 %INI% ？[y/N]: 
if /i "!DEL!"=="y" (
  del "%INI%"
  echo [完成] 已删除配置文件。
)

echo.
echo ============ 当前状态 ============
sc query RdHostService >nul 2>&1
if errorlevel 1 (
  echo RdHostService：已从服务管理器移除。
) else (
  echo RdHostService 仍在（可能标记删除，重启后彻底移除）：
  sc query RdHostService | find /i "STATE"
)
echo ==================================
pause
