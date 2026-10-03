$img = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $img | Get-Disk
$parts = $disk | Get-Partition
$out = @()
$out += "=== PARTITIONS ==="
foreach ($p in $parts) {
    $out += "Part $($p.PartitionNumber): DriveLetter=$($p.DriveLetter) Size=$($p.Size)"
}

# Find System Reserved (Boot partition) and OS partition
$bootPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Boot\BCD" }
if ($bootPart) {
    $bcdPath = "$($bootPart.DriveLetter):\Boot\BCD"
    $out += "=== BCD CONTENT ($bcdPath) ==="
    $bcdOut = & bcdedit /store "$bcdPath" /enum all
    $out += $bcdOut
}

$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
if ($osPart) {
    $hive = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"
    $out += "=== INJECTING MOUNTED DEVICES INTO $hive ==="
    & reg load HKLM\OFFLINE_SYS "$hive"
    Start-Sleep -Milliseconds 500

    $mdKey = "HKLM:\OFFLINE_SYS\MountedDevices"
    [byte[]]$cSig = 0x89, 0x84, 0x12, 0x3E, 0x00, 0x00, 0x60, 0x22, 0x00, 0x00, 0x00, 0x00
    [byte[]]$bootSig = 0x89, 0x84, 0x12, 0x3E, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00

    Set-ItemProperty -Path $mdKey -Name "\DosDevices\C:" -Value $cSig -Type Binary -Force
    Set-ItemProperty -Path $mdKey -Name "\??\Volume{3e128489-0000-0000-0000-602200000000}" -Value $cSig -Type Binary -Force
    Set-ItemProperty -Path $mdKey -Name "\??\Volume{3e128489-0000-0000-0000-100000000000}" -Value $bootSig -Type Binary -Force

    $out += "Successfully injected \DosDevices\C: into MountedDevices!"

    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg unload HKLM\OFFLINE_SYS
}

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\bcd_and_md_dump.txt" -Encoding utf8
