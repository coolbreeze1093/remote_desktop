# uninstall_host_service.ps1 - stop+uninstall RdHostService.
# Must run elevated PowerShell. The firewall rule is removed by the exe itself.
#
# Usage:
#   .\uninstall_host_service.ps1                 # keep rd_host.ini
#   .\uninstall_host_service.ps1 -RemoveConfig   # also delete rd_host.ini
#   .\uninstall_host_service.ps1 -Exe "D:\rd\rd_host_service.exe"
#Requires -RunAsAdministrator
param(
  [switch]$RemoveConfig,
  [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'

# --- locate rd_host_service.exe (same search order as install script)
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

& $Exe --uninstall --quiet
if ($LASTEXITCODE -ne 0) {
  throw "rd_host_service --uninstall failed (exit $LASTEXITCODE)"
}
Write-Host '[ok] --uninstall done'

Start-Sleep -Milliseconds 500
$svc = Get-Service -Name RdHostService -ErrorAction SilentlyContinue
if ($svc) {
  Write-Warning ("RdHostService still visible: status={0} (marked for delete; a reboot finishes removal)" -f $svc.Status)
} else {
  Write-Host '[ok] RdHostService removed from SCM'
}

if ($RemoveConfig -and (Test-Path $iniPath)) {
  Remove-Item $iniPath -Force
  Write-Host "[ok] removed $iniPath"
}
