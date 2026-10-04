# step2_patch.ps1
$ErrorActionPreference = "Continue"

Write-Host "--- 1. Set PagingFiles empty ---"
& reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet001\Control\Session Manager\Memory Management" /v PagingFiles /t REG_MULTI_SZ /d "\0" /f

Write-Host "--- 2. Delete MountedDevices ---"
& reg.exe delete "HKLM\OFFLINE_SYSTEM\MountedDevices" /va /f

Write-Host "--- 3. DisableDHCPMediaSense = 1 ---"
& reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet001\Services\Tcpip\Parameters" /v DisableDHCPMediaSense /t REG_DWORD /d 1 /f

Write-Host "--- 4. Check ControlSet002 ---"
$cs2 = & reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet002" 2>$null
if ($LASTEXITCODE -eq 0) {
    & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet002\Services\ms_wfplwf" /v Start /t REG_DWORD /d 4 /f
    & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet002\Control\Session Manager\Memory Management" /v PagingFiles /t REG_MULTI_SZ /d "\0" /f
    & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet002\Services\Tcpip\Parameters" /v DisableDHCPMediaSense /t REG_DWORD /d 1 /f
}

Write-Host "--- 5. Verify ---"
& reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet001\Services\ms_wfplwf" /v Start
& reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet001\Control\Session Manager\Memory Management" /v PagingFiles
& reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet001\Services\Tcpip\Parameters" /v DisableDHCPMediaSense
& reg.exe query "HKLM\OFFLINE_SYSTEM\MountedDevices"

Write-Host "--- 6. Unload Hive ---"
[gc]::Collect()
[gc]::WaitForPendingFinalizers()
Start-Sleep -Milliseconds 500

$unloaded = $false
for ($i = 1; $i -le 5; $i++) {
    & reg.exe unload HKLM\OFFLINE_SYSTEM
    if ($LASTEXITCODE -eq 0) {
        $unloaded = $true
        Write-Host "Hive unloaded successfully."
        break
    }
    Write-Host "Retrying unload ($i)..."
    Start-Sleep -Seconds 1
}

Write-Host "--- 7. Dismount VHD ---"
& diskpart.exe /s C:\Users\Admin\Desktop\cobdiskless\tools\dismount_cmd.txt
Write-Host "DONE."
