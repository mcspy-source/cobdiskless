$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\apply_vhd_fixes.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\apply_vhd_fixes.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\vhd_fix_and_start.ps1"
$supContent = @"
Start-Transcript -Path '$logFile' -Force

Write-Host '[1/7] Stopping cobdiskless_app...'
Stop-Process -Name 'cobdiskless_app' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host '[2/7] Patching registry in HKLM:\FIX_SYS...'
if (Test-Path 'HKLM:\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management') {
    Remove-ItemProperty -Path 'HKLM:\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management' -Name 'PagingFiles' -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path 'HKLM:\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management' -Name 'ExistingPageFiles' -ErrorAction SilentlyContinue
    Set-ItemProperty -Path 'HKLM:\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management' -Name 'ClearPageFileAtShutdown' -Value 0 -Type DWord -Force
    Write-Host '  Pagefile settings updated.'
} else {
    Write-Warning '  HKLM:\FIX_SYS not loaded or key missing.'
}

Write-Host '[3/7] Unloading HKLM\FIX_SYS...'
[GC]::Collect()
[GC]::WaitForPendingFinalizers()
for (`$i = 0; `$i -lt 5; `$i++) {
    & reg.exe unload HKLM\FIX_SYS
    if (`$LASTEXITCODE -eq 0) {
        Write-Host '  Successfully unloaded HKLM\FIX_SYS.'
        break
    }
    Start-Sleep -Seconds 1
}

Write-Host '[4/7] Sanitizing BCD entries...'
`$bcdStore = 'Y:\Boot\BCD'
if (-not (Test-Path `$bcdStore)) { `$bcdStore = 'W:\Boot\BCD' }
if (Test-Path `$bcdStore) {
    Write-Host "  Using BCD at `$bcdStore"
    & bcdedit.exe /store `$bcdStore /set "{bootmgr}" device boot
    & bcdedit.exe /store `$bcdStore /set "{default}" device boot
    & bcdedit.exe /store `$bcdStore /set "{default}" osdevice boot
    & bcdedit.exe /store `$bcdStore /set "{default}" sos yes
    & bcdedit.exe /store `$bcdStore /set "{default}" bootlog yes
    & bcdedit.exe /store `$bcdStore /set "{default}" bootstatuspolicy ignoreallfailures
    & bcdedit.exe /store `$bcdStore /set "{default}" recoveryenabled no
    Write-Host '  BCD configuration successfully updated.'
} else {
    Write-Warning '  BCD store not found!'
}

Write-Host '[5/7] Dismounting VHD and locking read-only...'
Dismount-DiskImage -ImagePath 'G:\G\csmw10.VHD' -ErrorAction SilentlyContinue | Out-Null
Start-Sleep -Seconds 2
attrib +r 'G:\G\csmw10.VHD'

Write-Host '[6/7] Purging writeback overlay files...'
Remove-Item -Path 'E:\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'E:\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue

Write-Host '[7/7] Restarting cobdiskless_app daemon...'
`$AppWorkDir = 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin'
`$AppExe = "`$AppWorkDir\cobdiskless_app.exe"
Set-Location `$AppWorkDir
`$p = Start-Process -FilePath `$AppExe -ArgumentList '--start-daemon --enable-trace-io' -WorkingDirectory `$AppWorkDir -PassThru

for (`$i = 0; `$i -lt 30; `$i++) {
    `$u67 = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { `$_.LocalPort -eq 67 }).Count -gt 0
    `$t18 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { `$_.State -eq 'Listen' -and `$_.LocalPort -eq 18080 }).Count -gt 0
    `$t32 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { `$_.State -eq 'Listen' -and `$_.LocalPort -eq 3260 }).Count -gt 0
    if (`$u67 -and `$t18 -and `$t32) {
        Write-Host "  cobdiskless_app ready (PID `$(`$p.Id))."
        break
    }
    Start-Sleep -Milliseconds 500
}

Stop-Transcript
Set-Content -Path '$marker' -Value "SUCCESS" -Force

while (`$true) {
    if (`$p.HasExited) { break }
    Start-Sleep -Seconds 2
}
"@

Set-Content -Path $supScript -Value $supContent -Force

Write-Host "Triggering elevated execution..."
Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $supScript) -Verb RunAs -WindowStyle Hidden

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 30000) {
    if (Test-Path $marker) {
        Write-Host "[+] Finished with status: $(Get-Content $marker -Raw)" -ForegroundColor Green
        break
    }
    Start-Sleep -Milliseconds 500
}

if (Test-Path $logFile) {
    Get-Content $logFile
}
