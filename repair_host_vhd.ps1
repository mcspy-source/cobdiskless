$ErrorActionPreference = "Stop"

# 1. Kill stale writeback overlays on root of E:\ and subfolders
Get-ChildItem -Path "E:\" -Filter "wb_*.cow" -File -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -Path "E:\writeback\" -Filter "*.cow" -File -ErrorAction SilentlyContinue | Remove-Item -Force
Write-Host "[+] Wiped all stale COW writeback overlays."

# 2. Fix the hive groups
$vhd = "G:\G\csmw10.vhd"
if (Test-Path $vhd) {
    Set-ItemProperty -Path $vhd -Name IsReadOnly -Value $false -ErrorAction SilentlyContinue

    $mount = Mount-DiskImage -ImagePath $vhd -PassThru
    Start-Sleep -Seconds 3
    $letter = ($mount | Get-Disk | Get-Partition | Get-Volume | Where-Object { $_.DriveLetter }).DriveLetter
    Write-Host "[+] Mounted VHD to drive ${letter}:"
    $hive = "${letter}:\Windows\System32\config\SYSTEM"

    & reg.exe load HKLM\FIX_SYS $hive

    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\ndis" /v Group /t REG_SZ /d "NDIS Wrapper" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\ndis" /v Start /t REG_DWORD /d 0 /f

    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\Tcpip" /v Group /t REG_SZ /d "PNP_TDI" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\Tcpip" /v Start /t REG_DWORD /d 0 /f

    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt" /v Group /t REG_SZ /d "SCSI Miniport" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt" /v Start /t REG_DWORD /d 0 /f

    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\msiscsi" /v Group /t REG_SZ /d "Boot Bus Extender" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\msiscsi" /v Start /t REG_DWORD /d 0 /f

    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\rt640x64" /v Group /t REG_SZ /d "NDIS" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\rt640x64" /v Start /t REG_DWORD /d 0 /f

    # Delete StartOverride subkeys
    try { & reg.exe delete "HKLM\FIX_SYS\ControlSet001\Services\ndis\StartOverride" /f } catch {}
    try { & reg.exe delete "HKLM\FIX_SYS\ControlSet001\Services\Tcpip\StartOverride" /f } catch {}
    try { & reg.exe delete "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt\StartOverride" /f } catch {}
    try { & reg.exe delete "HKLM\FIX_SYS\ControlSet001\Services\msiscsi\StartOverride" /f } catch {}
    try { & reg.exe delete "HKLM\FIX_SYS\ControlSet001\Services\rt640x64\StartOverride" /f } catch {}

    [GC]::Collect()
    Start-Sleep -Seconds 1
    & reg.exe unload HKLM\FIX_SYS
    Start-Sleep -Seconds 1
    Dismount-DiskImage -ImagePath $vhd
    Set-ItemProperty -Path $vhd -Name IsReadOnly -Value $true -ErrorAction SilentlyContinue
    Write-Host "[+] Master VHD driver hierarchy restored."
} else {
    Write-Host "[!] $vhd not found on host."
}
