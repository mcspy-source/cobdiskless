$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$bootPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Boot\BCD" }
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }

$out = @()
$out += "=== BOOT PARTITION: $($bootPart.DriveLetter) ==="
$out += "=== OS PARTITION: $($osPart.DriveLetter) ==="

$bcdPath = "$($bootPart.DriveLetter):\Boot\BCD"
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

# 1. BCD Inspection
$out += "=== BCDEDIT ENUM ALL ==="
$out += (& bcdedit /store "$bcdPath" /enum all /v)

# 2. SYSTEM Hive Inspection
& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out += "=== NET ADAPTER 0000 ALL PROPERTIES ==="
$instKey = "HKLM:\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0000"
if (Test-Path $instKey) {
    $props = Get-ItemProperty $instKey
    $props.PSObject.Properties | ForEach-Object {
        $out += "  $($_.Name) = $($_.Value)"
    }
}

$out += "=== SERVICES MSISCSI ==="
$msKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\msiscsi"
if (Test-Path $msKey) {
    (Get-ItemProperty $msKey).PSObject.Properties | ForEach-Object { $out += "  $($_.Name) = $($_.Value)" }
}

$out += "=== SERVICES TCPIP ==="
$tcpKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\Tcpip"
if (Test-Path $tcpKey) {
    (Get-ItemProperty $tcpKey).PSObject.Properties | ForEach-Object { $out += "  $($_.Name) = $($_.Value)" }
}

$out += "=== SERVICES NDIS ==="
$ndisKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\ndis"
if (Test-Path $ndisKey) {
    (Get-ItemProperty $ndisKey).PSObject.Properties | ForEach-Object { $out += "  $($_.Name) = $($_.Value)" }
}

$out += "=== CONTROL SESSION MANAGER ==="
$smKey = "HKLM:\OFFLINE_SYS\ControlSet001\Control\Session Manager"
if (Test-Path $smKey) {
    $sm = Get-ItemProperty $smKey
    $out += "  BootExecute = $($sm.BootExecute)"
}

$out += "=== MOUNTED DEVICES REG QUERY ==="
$out += (& reg query "HKLM\OFFLINE_SYS\MountedDevices")

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\deep_diag.txt" -Encoding utf8
