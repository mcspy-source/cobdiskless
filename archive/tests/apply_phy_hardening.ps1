$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$drive = "$($osPart.DriveLetter):"
$sysPath = "$drive\Windows\System32\config\SYSTEM"

$out = @()
$out += "Mount path: $sysPath"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

# 1. Services\rt640x64\Parameters: Inject Diskless and Link-Flap hardening
$rtParams = "HKLM\OFFLINE_SYS\ControlSet001\Services\rt640x64\Parameters"
& reg add $rtParams /v "Diskless" /t REG_DWORD /d 1 /f
& reg add $rtParams /v "DisklessOption" /t REG_DWORD /d 1 /f
& reg add $rtParams /v "WaitAutoNegComplete" /t REG_DWORD /d 0 /f
& reg add $rtParams /v "*EEE" /t REG_SZ /d "0" /f
& reg add $rtParams /v "AutoPowerSave" /t REG_SZ /d "0" /f
& reg add $rtParams /v "GigaLite" /t REG_SZ /d "0" /f
& reg add $rtParams /v "*FlowControl" /t REG_SZ /d "0" /f
& reg add $rtParams /v "PowerSavingMode" /t REG_SZ /d "0" /f
& reg add $rtParams /v "EnableGreenEthernet" /t REG_SZ /d "0" /f
& reg add $rtParams /v "AutoDisableGigabit" /t REG_SZ /d "0" /f
& reg add $rtParams /v "PowerDownPll" /t REG_DWORD /d 0 /f
& reg add $rtParams /v "S5NicKeepOverrideMacAddr" /t REG_DWORD /d 1 /f
& reg add $rtParams /v "S5NicKeepOverrideMacAddrV2" /t REG_DWORD /d 1 /f
& reg add $rtParams /v "*SpeedDuplex" /t REG_SZ /d "0" /f

# Also inject into other vendor NIC parameters just in case
foreach ($nic in @("rt68cx64", "e1d68x64", "e2f68")) {
    $np = "HKLM\OFFLINE_SYS\ControlSet001\Services\$nic\Parameters"
    & reg add $np /v "Diskless" /t REG_DWORD /d 1 /f
    & reg add $np /v "DisklessOption" /t REG_DWORD /d 1 /f
    & reg add $np /v "WaitAutoNegComplete" /t REG_DWORD /d 0 /f
    & reg add $np /v "*EEE" /t REG_SZ /d "0" /f
    & reg add $np /v "AutoPowerSave" /t REG_SZ /d "0" /f
    & reg add $np /v "GigaLite" /t REG_SZ /d "0" /f
    & reg add $np /v "*FlowControl" /t REG_SZ /d "0" /f
}

# 2. Net Class 0000: Set WaitAutoNegComplete to 0
$net0 = "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0000"
& reg add $net0 /v "WaitAutoNegComplete" /t REG_DWORD /d 0 /f
& reg add $net0 /v "Diskless" /t REG_DWORD /d 1 /f
& reg add $net0 /v "DisklessOption" /t REG_DWORD /d 1 /f
& reg add $net0 /v "*EEE" /t REG_SZ /d "0" /f
& reg add $net0 /v "PowerSavingMode" /t REG_SZ /d "0" /f
& reg add $net0 /v "EnableGreenEthernet" /t REG_SZ /d "0" /f
& reg add $net0 /v "AutoPowerSave" /t REG_SZ /d "0" /f
& reg add $net0 /v "GigaLite" /t REG_SZ /d "0" /f
& reg add $net0 /v "AutoDisableGigabit" /t REG_SZ /d "0" /f
& reg add $net0 /v "PowerDownPll" /t REG_DWORD /d 0 /f
& reg add $net0 /v "*FlowControl" /t REG_SZ /d "0" /f

# 3. Disable MSI for Realtek NIC (prevent silent interrupt drops on BIOS CSM)
$msiKey = "HKLM\OFFLINE_SYS\ControlSet001\Enum\PCI\VEN_10EC&DEV_8168&SUBSYS_307C17AA&REV_06\01000000684CE00000\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties"
& reg add $msiKey /v "MSISupported" /t REG_DWORD /d 0 /f

# 4. iSCSI SCSIAdapter Class Parameters
$scsi0 = "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}\0000\Parameters"
& reg add $scsi0 /v "LinkDownTime" /t REG_DWORD /d 60 /f
& reg add $scsi0 /v "MaxRequestHoldTime" /t REG_DWORD /d 60 /f

# 5. Services\msiscsi\Parameters and iScsiPrt\Parameters
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\msiscsi\Parameters" /v "LinkDownTime" /t REG_DWORD /d 60 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\msiscsi\Parameters" /v "MaxRequestHoldTime" /t REG_DWORD /d 60 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "LinkDownTime" /t REG_DWORD /d 60 /f
& reg add "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "MaxRequestHoldTime" /t REG_DWORD /d 60 /f
& reg delete "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "WaitForNetworkAtBoot" /f 2>$null

# 6. Verification
$out += "=== rt640x64 Parameters ==="
$out += (& reg query $rtParams /s)
$out += "=== Net Class 0000 ==="
$out += (& reg query $net0 /v WaitAutoNegComplete)
$out += (& reg query $net0 /v Diskless)
$out += "=== MSI Supported ==="
$out += (& reg query $msiKey /v MSISupported)
$out += "=== SCSI 0000 Parameters ==="
$out += (& reg query $scsi0)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS

# Clean dirty logs
$cfgDir = Split-Path $sysPath
foreach ($log in @("SYSTEM.LOG", "SYSTEM.LOG1", "SYSTEM.LOG2")) {
    $lp = Join-Path $cfgDir $log
    if (Test-Path $lp) {
        Remove-Item -Path $lp -Force -ErrorAction SilentlyContinue
        $out += "Cleaned log: $log"
    }
}

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\apply_phy_hardening.txt" -Encoding utf8
