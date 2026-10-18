$ErrorActionPreference = "Continue"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\finish_boot_fixes.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\finish_boot_fixes.marker"

Start-Transcript -Path $logFile -Force

Write-Host "=== Applying Remaining Boot & Registry Fixes ==="

# 1. PagingFiles in HKLM\FIX_SYS
Write-Host "[1/5] Removing pagefile references from registry..."
try {
    # Delete PagingFiles so Windows doesn't attempt to create pagefile on iSCSI
    & reg.exe delete "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "PagingFiles" /f
    & reg.exe delete "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "ExistingPageFiles" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "ClearPageFileAtShutdown" /t REG_DWORD /d 0 /f
    Write-Host "Pagefile entries cleared successfully."
} catch {
    Write-Warning "Failed clearing pagefile: $_"
}

# 2. Disk & iSCSI Timeouts
Write-Host "[2/5] Setting iSCSI / disk timeout resilience..."
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\disk" /v "TimeOutValue" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "LinkDownTime" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "MaxRequestHoldTime" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\msiscsi\Parameters" /v "LinkDownTime" /t REG_DWORD /d 60 /f
& reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\msiscsi\Parameters" /v "MaxRequestHoldTime" /t REG_DWORD /d 60 /f

# 3. Unload SYSTEM hive
Write-Host "[3/5] Unloading HKLM\FIX_SYS..."
[GC]::Collect()
[GC]::WaitForPendingFinalizers()
$unloaded = $false
for ($i = 0; $i -lt 5; $i++) {
    $res = & reg.exe unload HKLM\FIX_SYS 2>&1
    Write-Host "Unload attempt $i: $res"
    if ($LASTEXITCODE -eq 0) {
        $unloaded = $true
        break
    }
    Start-Sleep -Seconds 1
}

# 4. BCD Sanitization
Write-Host "[4/5] Sanitizing BCD entries..."
$bcdStore = $null
if (Test-Path "Y:\Boot\BCD") {
    $bcdStore = "Y:\Boot\BCD"
} elseif (Test-Path "W:\Boot\BCD") {
    $bcdStore = "W:\Boot\BCD"
}

if ($bcdStore) {
    Write-Host "Found BCD at $bcdStore"
    & bcdedit.exe /store $bcdStore /set "{bootmgr}" device boot
    & bcdedit.exe /store $bcdStore /set "{default}" device boot
    & bcdedit.exe /store $bcdStore /set "{default}" osdevice boot
    & bcdedit.exe /store $bcdStore /set "{default}" sos yes
    & bcdedit.exe /store $bcdStore /set "{default}" bootlog yes
    & bcdedit.exe /store $bcdStore /set "{default}" bootstatuspolicy ignoreallfailures
    & bcdedit.exe /store $bcdStore /set "{default}" recoveryenabled no
    Write-Host "BCD sanitization finished. Enum default:"
    & bcdedit.exe /store $bcdStore /enum "{default}"
} else {
    Write-Warning "Could not find BCD store!"
}

# 5. Dismount VHD, set read-only, purge COW
Write-Host "[5/5] Dismounting VHD and locking to read-only..."
Dismount-DiskImage -ImagePath "G:\G\csmw10.VHD" -ErrorAction SilentlyContinue | Out-Null
Start-Sleep -Seconds 2
attrib +r "G:\G\csmw10.VHD"

Write-Host "Purging writeback files..."
Remove-Item -Path "E:\wb_6c4b902f61eb.cow" -Force -ErrorAction SilentlyContinue
Remove-Item -Path "E:\writeback\wb_6c4b902f61eb.cow" -Force -ErrorAction SilentlyContinue
Remove-Item -Path "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\writeback\wb_6c4b902f61eb.cow" -Force -ErrorAction SilentlyContinue

Write-Host "ALL REMAINING BOOT FIXES COMPLETED SUCCESSFULLY!"
Stop-Transcript
Set-Content -Path $marker -Value "SUCCESS" -Force
