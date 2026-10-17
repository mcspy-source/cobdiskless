$vhd = "G:\G\csmw10.VHD"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\apply_boot_fixes.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\apply_boot_fixes.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\apply_boot_fixes_run.ps1"
$supContent = @"
`$vhd = '$vhd'
`$logFile = '$logFile'
`$marker = '$marker'

Start-Transcript -Path `$logFile -Force

Write-Host "[1/6] Unlocking VHD read-only attribute..."
attrib -r `$vhd

Write-Host "[2/6] Mounting VHD in Read/Write mode..."
Mount-DiskImage -ImagePath `$vhd -Access ReadWrite | Out-Null
Start-Sleep -Seconds 2

`$d = Get-DiskImage -ImagePath `$vhd | Get-Disk
`$parts = Get-Partition -DiskNumber `$d.Number | Where-Object { `$_.Type -ne 'Reserved' }

`$osLetter = `$null
`$bootLetter = `$null

foreach (`$p in `$parts) {
    if (`$p.DriveLetter) {
        `$letter = "`$(`$p.DriveLetter):"
        if (Test-Path "`$letter\Windows\System32\config\SYSTEM") { `$osLetter = `$letter }
        if (Test-Path "`$letter\Boot\BCD") { `$bootLetter = `$letter }
    } else {
        # Assign drive letter if missing
        for (`$c = [char]'Y'; `$c -ge [char]'K'; `$c--) {
            `$cand = "`$([char]`$c):"
            if (-not (Get-PSDrive -Name ([char]`$c) -ErrorAction SilentlyContinue)) {
                Set-Partition -DiskNumber `$d.Number -PartitionNumber `$p.PartitionNumber -NewDriveLetter ([char]`$c) -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 500
                if (Test-Path "`$cand\Windows\System32\config\SYSTEM") { `$osLetter = `$cand; break }
                if (Test-Path "`$cand\Boot\BCD") { `$bootLetter = `$cand; break }
                break
            }
        }
    }
}

Write-Host "  Boot Partition: `$bootLetter"
Write-Host "  OS Partition:   `$osLetter"

if (-not `$osLetter) {
    Write-Error "Could not locate OS partition!"
    Stop-Transcript
    Set-Content -Path `$marker -Value "FAIL_NO_OS" -Force
    exit 1
}

# --- Registry Adjustments ---
Write-Host "[3/6] Loading SYSTEM hive from `$osLetter\Windows\System32\config\SYSTEM..."
& reg.exe load HKLM\FIX_SYS "`$osLetter\Windows\System32\config\SYSTEM"

Write-Host "  Patching CrashControl: AutoReboot=0, CrashDumpEnabled=0..."
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\CrashControl" /v "AutoReboot" /t REG_DWORD /d 0 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\CrashControl" /v "CrashDumpEnabled" /t REG_DWORD /d 0 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\CrashControl" /v "LogEvent" /t REG_DWORD /d 1 /f

Write-Host "  Disabling pagefile on iSCSI boot..."
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "PagingFiles" /t REG_MULTI_SZ /d "" /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "ExistingPageFiles" /t REG_MULTI_SZ /d "" /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "ClearPageFileAtShutdown" /t REG_DWORD /d 0 /f

Write-Host "  Ensuring iScsiPrt / disk timeout resilience..."
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\disk" /v "TimeOutValue" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "LinkDownTime" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "MaxRequestHoldTime" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\msiscsi\Parameters" /v "LinkDownTime" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\msiscsi\Parameters" /v "MaxRequestHoldTime" /t REG_DWORD /d 60 /f

Write-Host "  Unloading SYSTEM hive..."
[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg.exe unload HKLM\FIX_SYS

# --- BCD Adjustments ---
if (`$bootLetter -and (Test-Path "`$bootLetter\Boot\BCD")) {
    `$bcdStore = "`$bootLetter\Boot\BCD"
    Write-Host "[4/6] Sanitizing BCD entries in `$bcdStore..."
    
    # Set bootmgr device
    & bcdedit.exe /store `$bcdStore /set "{bootmgr}" device boot
    
    # Set default loader device and osdevice to boot
    & bcdedit.exe /store `$bcdStore /set "{default}" device boot
    & bcdedit.exe /store `$bcdStore /set "{default}" osdevice boot
    
    # Enable SOS display and bootlog
    & bcdedit.exe /store `$bcdStore /set "{default}" sos yes
    & bcdedit.exe /store `$bcdStore /set "{default}" bootlog yes
    & bcdedit.exe /store `$bcdStore /set "{default}" bootstatuspolicy ignoreallfailures
    & bcdedit.exe /store `$bcdStore /set "{default}" recoveryenabled no
    
    Write-Host "  Updated BCD entries successfully!"
    & bcdedit.exe /store `$bcdStore /enum "{default}"
} else {
    Write-Warning "Could not find BCD store to sanitize."
}

Write-Host "[5/6] Dismounting VHD and locking to read-only..."
Dismount-DiskImage -ImagePath `$vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r `$vhd

Write-Host "[6/6] Purging writeback COW overlays..."
Remove-Item -Path "E:\wb_6c4b902f61eb.cow" -Force -ErrorAction SilentlyContinue
Remove-Item -Path "E:\writeback\wb_6c4b902f61eb.cow" -Force -ErrorAction SilentlyContinue
Remove-Item -Path "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\writeback\wb_6c4b902f61eb.cow" -Force -ErrorAction SilentlyContinue

Write-Host "All boot fixes successfully applied!"
Stop-Transcript
Set-Content -Path `$marker -Value "SUCCESS" -Force
"@
Set-Content -Path $supScript -Value $supContent -Force

Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $supScript) -Verb RunAs -WindowStyle Hidden -PassThru | Out-Null

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 45000) {
    if (Test-Path $marker) {
        Write-Host "Completed with status: $(Get-Content $marker -Raw)"
        break
    }
    Start-Sleep -Milliseconds 500
}

if (Test-Path $logFile) {
    Get-Content $logFile
}
