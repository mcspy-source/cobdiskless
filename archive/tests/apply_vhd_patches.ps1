$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== APPLYING SYSTEM HIVE PATCHES TO $sysPath ==="

# -------------------------------------------------------------
# 1. Register iScsiPrt (The actual kernel driver for msiscsi.sys)
# -------------------------------------------------------------
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "DisplayName" /t REG_SZ /d "iScsiPort Driver" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "ImagePath" /t REG_EXPAND_SZ /d "System32\drivers\msiscsi.sys" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "Type" /t REG_DWORD /d 1 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "Start" /t REG_DWORD /d 0 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "ErrorControl" /t REG_DWORD /d 1 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "Group" /t REG_SZ /d "CCiSCSI" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "BootFlags" /t REG_DWORD /d 1 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /v "Owners" /t REG_MULTI_SZ /d "iscsi.inf" /f

& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "BusType" /t REG_DWORD /d 9 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "WaitForNetworkAtBoot" /t REG_DWORD /d 0 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "DelayForNetworkAtBoot" /t REG_DWORD /d 1 /f

& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Enum" /v "0" /t REG_SZ /d "ROOT\ISCSIPRT\0000" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Enum" /v "Count" /t REG_DWORD /d 1 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Enum" /v "NextInstance" /t REG_DWORD /d 1 /f

# -------------------------------------------------------------
# 2. Register ROOT\ISCSIPRT\0000 Enum device
# -------------------------------------------------------------
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000" /v "ClassGUID" /t REG_SZ /d "{4d36e97b-e325-11ce-bfc1-08002be10318}" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000" /v "HardwareID" /t REG_MULTI_SZ /d "ROOT\ISCSIPRT" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000" /v "Service" /t REG_SZ /d "iScsiPrt" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000" /v "DeviceDesc" /t REG_SZ /d "Microsoft iSCSI Initiator" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000" /v "Driver" /t REG_SZ /d "{4d36e97b-e325-11ce-bfc1-08002be10318}\0000" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000" /v "Mfg" /t REG_SZ /d "Microsoft" /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000" /v "ConfigFlags" /t REG_DWORD /d 0 /f

# -------------------------------------------------------------
# 3. Register SCSIAdapter Class 0000 for iSCSI Initiator
# -------------------------------------------------------------
$scsiInst = "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}\0000"
& reg add $scsiInst /v "DriverDesc" /t REG_SZ /d "Microsoft iSCSI Initiator" /f
& reg add $scsiInst /v "ProviderName" /t REG_SZ /d "Microsoft" /f
& reg add $scsiInst /v "InfPath" /t REG_SZ /d "iscsi.inf" /f
& reg add $scsiInst /v "MatchingDeviceId" /t REG_SZ /d "ROOT\ISCSIPRT" /f

# -------------------------------------------------------------
# 4. Patch Realtek Net Class instance (0000): Stop PHY renegotiation & EEE link reset
# -------------------------------------------------------------
$netInst = "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0000"
& reg add $netInst /v "Diskless" /t REG_DWORD /d 1 /f
& reg add $netInst /v "DisklessOption" /t REG_DWORD /d 1 /f
& reg add $netInst /v "WaitAutoNegComplete" /t REG_DWORD /d 1 /f
& reg add $netInst /v "*EEE" /t REG_SZ /d "0" /f
& reg add $netInst /v "PowerSavingMode" /t REG_SZ /d "0" /f
& reg add $netInst /v "EnableGreenEthernet" /t REG_SZ /d "0" /f
& reg add $netInst /v "AutoPowerSave" /t REG_SZ /d "0" /f
& reg add $netInst /v "GigaLite" /t REG_SZ /d "0" /f
& reg add $netInst /v "AutoDisableGigabit" /t REG_SZ /d "0" /f
& reg add $netInst /v "PowerDownPll" /t REG_DWORD /d 0 /f
& reg add $netInst /v "*FlowControl" /t REG_SZ /d "0" /f
& reg add $netInst /v "S5NicKeepOverrideMacAddr" /t REG_DWORD /d 1 /f
& reg add $netInst /v "S5NicKeepOverrideMacAddrV2" /t REG_DWORD /d 1 /f

# -------------------------------------------------------------
# 5. Populate CriticalDeviceDatabase for iSCSI and Realtek
# -------------------------------------------------------------
$cddb = "HKLM\OFFLINE_SYS\ControlSet001\Control\CriticalDeviceDatabase"
& reg add "$cddb\root#iscsiprt" /v "ClassGUID" /t REG_SZ /d "{4d36e97b-e325-11ce-bfc1-08002be10318}" /f
& reg add "$cddb\root#iscsiprt" /v "Service" /t REG_SZ /d "iScsiPrt" /f

& reg add "$cddb\pci#ven_10ec&dev_8168" /v "ClassGUID" /t REG_SZ /d "{4d36e972-e325-11ce-bfc1-08002be10318}" /f
& reg add "$cddb\pci#ven_10ec&dev_8168" /v "Service" /t REG_SZ /d "rt640x64" /f

& reg add "$cddb\pci#ven_10ec&dev_8168&rev_06" /v "ClassGUID" /t REG_SZ /d "{4d36e972-e325-11ce-bfc1-08002be10318}" /f
& reg add "$cddb\pci#ven_10ec&dev_8168&rev_06" /v "Service" /t REG_SZ /d "rt640x64" /f

& reg add "$cddb\pci#ven_10ec&cc_0200" /v "ClassGUID" /t REG_SZ /d "{4d36e972-e325-11ce-bfc1-08002be10318}" /f
& reg add "$cddb\pci#ven_10ec&cc_0200" /v "Service" /t REG_SZ /d "rt640x64" /f

& reg add "$cddb\pci#ven_10ec&cc_020000" /v "ClassGUID" /t REG_SZ /d "{4d36e972-e325-11ce-bfc1-08002be10318}" /f
& reg add "$cddb\pci#ven_10ec&cc_020000" /v "Service" /t REG_SZ /d "rt640x64" /f

# -------------------------------------------------------------
# 6. Ensure MountedDevices \DosDevices\C: is verified
# -------------------------------------------------------------
& reg add "HKLM\OFFLINE_SYS\MountedDevices" /v "\DosDevices\C:" /t REG_BINARY /d "8984123E0000602200000000" /f
& reg add "HKLM\OFFLINE_SYS\MountedDevices" /v "\??\Volume{3e128489-0000-0000-0000-602200000000}" /t REG_BINARY /d "8984123E0000602200000000" /f
& reg add "HKLM\OFFLINE_SYS\MountedDevices" /v "\??\Volume{3e128489-0000-0000-0000-100000000000}" /t REG_BINARY /d "8984123E0000100000000000" /f

# -------------------------------------------------------------
# 7. Verification Query
# -------------------------------------------------------------
$out += "=== VERIFY ISCSIPRT ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /s)
$out += "=== VERIFY CDDB ==="
$out += (& reg query "$cddb" /s)
$out += "=== VERIFY NET 0000 ==="
$out += (& reg query "$netInst")
$out += "=== VERIFY MOUNTED DEVICES ==="
$out += (& reg query "HKLM\OFFLINE_SYS\MountedDevices")

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS

# -------------------------------------------------------------
# 8. Clean up any dirty transaction logs in config dir
# -------------------------------------------------------------
$cfgDir = Split-Path $sysPath
foreach ($log in @("SYSTEM.LOG", "SYSTEM.LOG1", "SYSTEM.LOG2")) {
    $lp = Join-Path $cfgDir $log
    if (Test-Path $lp) {
        Remove-Item -Path $lp -Force -ErrorAction SilentlyContinue
        $out += "Removed dirty log: $log"
    }
}

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\patch_vhd_result.txt" -Encoding utf8
