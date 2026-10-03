$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32" }
$dl = $osPart.DriveLetter
$out = @()
& reg load HKLM\OFFLINE_SYS "$dl`:\Windows\System32\config\SYSTEM"

$filters = @("{EA24CD6C-D17A-4348-9190-09F0D5BE83DD}", "{3BFD7820-D65C-4C1B-9FEA-983A019639EA}", "{B70D6460-3635-4D42-B866-B8AB1A24454C}")
foreach ($f in $filters) {
    $out += "=== Filter GUID: $f ==="
    $out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Network" /s /f "$f" 2>&1)
}

$out += "=== Adapter 0000 UpperBind ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}\0000\Linkage" /v "UpperBind" 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\filter_details.txt" -Encoding utf8
