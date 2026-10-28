@echo off
rem install_host_service.bat - write rd_host.ini (port/stun) then install and
rem start the RdHostService service. Made for double-click: it elevates itself
rem via UAC and asks port/stun interactively (Enter = default).
setlocal EnableExtensions EnableDelayedExpansion
rem 本文件必须以 GBK(936) 编码保存：中文 Windows 的 cmd 默认按 936 解析，
rem 双击运行中文显示/解析都正确；不要转存为 UTF-8 或带 BOM。

rem ---- 自提权：双击默认非管理员，先重新以管理员启动自己 ----
net session >nul 2>&1
if errorlevel 1 (
  echo 需要管理员权限，正在弹出 UAC 确认窗口...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
title RemoteDesktop 被控服务安装

rem ---- 定位 rd_host_service.exe：本 bat 同目录，或 ..\publish ----
set "EXE="
if exist "%~dp0rd_host_service.exe" set "EXE=%~dp0rd_host_service.exe"
if not defined EXE if exist "%~dp0..\publish\rd_host_service.exe" set "EXE=%~dp0..\publish\rd_host_service.exe"
if not defined EXE (
  echo [错误] 找不到 rd_host_service.exe。
  echo 请把本 bat 放到 rd_host_service.exe 同目录，或放到仓库 scripts\ 目录。
  pause
  exit /b 1
)

rem ---- 询问参数（直接回车 = 默认值）----
echo 被控服务安装器：写入 rd_host.ini 并安装/启动 RdHostService。
echo 不确定就一路回车。
echo.
set "PORT="
set /p PORT=信令监听端口 [默认 48010]: 
if not defined PORT set "PORT=48010"
echo(%PORT%|findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 (
  echo [错误] 端口必须是纯数字：%PORT%
  pause
  exit /b 1
)
set "STUN="
set /p STUN=STUN 服务器 [默认 stun.miwifi.com:3478，填 off 禁用]: 
if not defined STUN set "STUN=stun.miwifi.com:3478"

rem ---- 写 rd_host.ini（exe 同目录；服务与 helper 每次启动都读）----
for %%F in ("%EXE%") do set "INI=%%~dpFrd_host.ini"
(
  echo # rd_host_service config - lives next to rd_host_service.exe
  echo # port: signaling listen port ^(1-65535^), default 48010
  echo # stun: ICE STUN server^(s^), comma separated; "off" disables STUN.
  echo # Effective after service ^(re^)start. Rerun this installer to change.
  echo port=%PORT%
  echo stun=%STUN%
) > "%INI%"
echo [完成] 已写配置：%INI%

rem ---- 记录安装前服务是否在跑（在跑则装完重启以应用新配置）----
set "WASRUNNING=0"
sc query RdHostService >nul 2>&1
if not errorlevel 1 (
  sc query RdHostService | find /i "RUNNING" >nul && set "WASRUNNING=1"
)

echo 正在安装服务...
"%EXE%" --install --quiet
if errorlevel 1 (
  echo [错误] rd_host_service --install 失败（退出码 %errorlevel%）。
  echo 可看 %INI% 同目录 rd_debug.log 里的 [service] 行。
  pause
  exit /b 1
)
echo [完成] --install 成功。

if "!WASRUNNING!"=="1" (
  echo 服务之前已在运行，正在重启以应用新配置...
  net stop RdHostService >nul 2>&1
  timeout /t 1 /nobreak >nul
  net start RdHostService >nul
)

echo.
echo ============ 当前状态 ============
sc query RdHostService | find /i "STATE"
echo 配置文件：%INI%
type "%INI%"
echo ==================================
echo 主控端请连接本机 IP 的端口 %PORT%。
echo 窗口可以关闭了。
pause
