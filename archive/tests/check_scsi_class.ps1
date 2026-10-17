$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== SCSI CLASS ADAPTERS ==="
$scsiKey = "HKLM:\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}"
if (Test-Path $scsiKey) {
    Get-ChildItem $scsiKey | ForEach-Object {
        $props = Get-ItemProperty $_.PSPath
        $out += "  $($_.PSChildName) : DriverDesc=$($props.DriverDesc) MatchingDeviceId=$($props.MatchingDeviceId) ProviderName=$($props.ProviderName)"
    }
} else {
    $out += "SCSI Class key not found!"
}

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\scsi_class_check.txt" -Encoding utf8
