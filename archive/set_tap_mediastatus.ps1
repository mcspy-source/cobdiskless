# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "set_tap_mediastatus.log"
Start-Transcript -Path $logPath -Force

$key = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0011"
Set-ItemProperty -Path $key -Name "MediaStatus" -Value "1" -Type String
Write-Host "Set MediaStatus to 1 on $key"

# Restart TAP adapter to apply
Restart-NetAdapter -Name "OpenVPN TAP-Windows6" -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

Get-NetAdapter "OpenVPN TAP-Windows6" | Format-Table -AutoSize
Stop-Transcript
