$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$drive = "$($osPart.DriveLetter):"
$sysPath = "$drive\Windows\System32\config\SYSTEM"

$res = @()
$res += "OS Partition Drive: $drive"
$res += "System Path: $sysPath"

# Check driver files
$filesToCheck = @(
    "Windows\System32\drivers\Wdf01000.sys",
    "Windows\System32\drivers\WDFLDR.SYS",
    "Windows\System32\drivers\rt640x64.sys",
    "Windows\System32\drivers\msiscsi.sys",
    "Windows\System32\drivers\tcpip.sys",
    "Windows\System32\drivers\ndis.sys"
)
foreach ($f in $filesToCheck) {
    $full = Join-Path $drive $f
    $exists = Test-Path $full
    $res += "File $f : $exists"
}

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$res += "=== Services Wdf01000 ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\Wdf01000" /s 2>&1)

$res += "=== Services WDFLDR ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\WDFLDR" /s 2>&1)

$res += "=== Services rt640x64 ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\rt640x64" /s 2>&1)

$res += "=== ServiceGroupOrder List ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\ServiceGroupOrder" /v List 2>&1)

$res += "=== BootExecute ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Session Manager" /v BootExecute 2>&1)

$res += "=== Net Class 0000 ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0000" 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_wdf_diag.txt" -Encoding utf8
