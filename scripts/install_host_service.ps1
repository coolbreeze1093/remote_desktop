# install_host_service.ps1 - write rd_host.ini then install+start RdHostService.
# Windows services cannot be installed from .sh; this must run elevated PowerShell.
#
# Usage:
#   .\install_host_service.ps1                                  # defaults: port 48010, stun.miwifi.com:3478
#   .\install_host_service.ps1 -Port 48011                      # custom signaling port
#   .\install_host_service.ps1 -Stun "stun.a.com:3478,stun.b.com:3478"
#   .\install_host_service.ps1 -Stun off                        # disable STUN
#   .\install_host_service.ps1 -Exe "D:\rd\rd_host_service.exe" # exe outside default locations
#
# The ini is written next to rd_host_service.exe; the service and its session
# helper both read it (port = signaling listen port, stun = ICE STUN servers).
# Changing port/stun later: rerun this script, it restarts the running service.
#Requires -RunAsAdministrator
param(
  [ushort]$Port = 48010,
  [string]$Stun = 'stun.miwifi.com:3478',
  [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'

# --- locate rd_host_service.exe: -Exe, else beside this script, else ..\publish
if (-not $Exe) {
  $candidates = @(
    (Join-Path $PSScriptRoot 'rd_host_service.exe'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'publish\rd_host_service.exe')
  )
  foreach ($c in $candidates) {
    if (Test-Path $c) { $Exe = $c; break }
  }
}
if (-not $Exe -or -not (Test-Path $Exe)) {
  throw 'rd_host_service.exe not found; pass -Exe <full path>.'
}
$Exe = (Resolve-Path $Exe).Path
$iniPath = Join-Path (Split-Path $Exe -Parent) 'rd_host.ini'
Write-Host "[..] target exe: $Exe"

# --- write rd_host.ini (UTF-8 w/ BOM; the C++ parser skips the BOM)
$lines = @(
  '# rd_host_service config - lives next to rd_host_service.exe',
  '# port: signaling listen port (1-65535), default 48010',
  '# stun: ICE STUN server(s) for the host side, comma separated;',
  '#      "off" (or empty) disables STUN. Effective after service (re)start.'
  "port=$Port"
)
if ($Stun -match '^(?i)(off|none|disabled)$') {
  $lines += 'stun=off'
} else {
  $lines += "stun=$Stun"
}
$lines += ''
Set-Content -Path $iniPath -Value $lines -Encoding UTF8
Write-Host "[ok] config written: $iniPath"
Get-Content $iniPath | ForEach-Object { Write-Host "     $_" }

# --- install (idempotent: --install also starts a freshly created service)
$svc = Get-Service -Name RdHostService -ErrorAction SilentlyContinue
$wasRunning = ($svc -and $svc.Status -eq 'Running')

& $Exe --install --quiet
if ($LASTEXITCODE -ne 0) {
  throw "rd_host_service --install failed (exit $LASTEXITCODE)"
}
Write-Host '[ok] --install done'

# --- restart if it was already running, so the new port/stun takes effect
$svc = Get-Service -Name RdHostService -ErrorAction SilentlyContinue
if ($svc) {
  if ($wasRunning) {
    Write-Host '[..] service was already running - restarting to apply new config'
    Restart-Service -Name RdHostService -Force
  }
  $svc.Refresh()
  Write-Host ("[ok] RdHostService: status={0} startType={1}" -f
    $svc.Status, (Get-Service RdHostService).StartType)
  Write-Host ("     signaling port {0}, config {1}" -f $Port, $iniPath)
} else {
  Write-Warning 'RdHostService not visible in SCM right after install.'
}
