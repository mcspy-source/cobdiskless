$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$classKey = "HKLM:\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}"
$out = @()
$out += "=== NET ADAPTER CLASS INSTANCES ==="
if (Test-Path $classKey) {
    $instances = Get-ChildItem $classKey | Where-Object { $_.PSChildName -match '^\d{4}$' }
    foreach ($inst in $instances) {
        $props = Get-ItemProperty $inst.PSPath
        $out += "Instance: $($inst.PSChildName)"
        $out += "  DriverDesc: $($props.DriverDesc)"
        $out += "  MatchingDeviceId: $($props.MatchingDeviceId)"
        $out += "  ComponentId: $($props.ComponentId)"
        $out += "  NetCfgInstanceId: $($props.NetCfgInstanceId)"
        $out += "  *SpeedDuplex: $($props.'*SpeedDuplex')"
        $out += "  WaitAutoNegComplete: $($props.WaitAutoNegComplete)"
        $out += "  *EEE: $($props.'*EEE')"
        $out += "  GigaLite: $($props.GigaLite)"
        $out += "  AutoPowerSave: $($props.AutoPowerSave)"
    }
} else {
    $out += "Class key not found!"
}

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\net_instances_dump.txt" -Encoding utf8
