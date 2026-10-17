$img = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $img | Get-Disk
$parts = $disk | Get-Partition
$vols = $parts | Get-Volume -ErrorAction SilentlyContinue
$out = @()
$out += "=== DISK PARTITIONS & VOLUMES ==="
foreach ($p in $parts) {
    $v = $p | Get-Volume -ErrorAction SilentlyContinue
    $out += "Part $($p.PartitionNumber): DriveLetter=$($p.DriveLetter) Size=$($p.Size) Label=$($v.FileSystemLabel)"
}

$hive = $null
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $p_sys = "$($p.DriveLetter):\Windows\System32\config\SYSTEM"
        if (Test-Path $p_sys) { $hive = $p_sys; break }
    }
}
$out += "Hive Path: $hive"

if ($hive) {
    # Check driver files on disk
    $volRoot = Split-Path (Split-Path (Split-Path (Split-Path $hive)))
    $out += "Volume Root: $volRoot"
    $driversDir = Join-Path $volRoot "Windows\System32\drivers"
    $out += "=== DRIVERS IN $driversDir ==="
    foreach ($drv in @("rt640x64.sys", "rt68cx64.sys", "e1d68x64.sys", "e2f68.sys", "msiscsi.sys", "ndis.sys", "tcpip.sys")) {
        $dp = Join-Path $driversDir $drv
        $exists = Test-Path $dp
        $size = if ($exists) { (Get-Item $dp).Length } else { 0 }
        $out += "  $drv : Exists=$exists Size=$size"
    }

    # Load hive into registry
    & reg load HKLM\OFFLINE_SYS "$hive"
    Start-Sleep -Milliseconds 500

    $out += "=== HKLM\OFFLINE_SYS SUBKEYS ==="
    $keys = Get-ChildItem HKLM:\OFFLINE_SYS
    foreach ($k in $keys) { $out += "  $($k.PSChildName)" }

    $out += "=== MOUNTED DEVICES ==="
    $mdKey = "HKLM:\OFFLINE_SYS\MountedDevices"
    if (Test-Path $mdKey) {
        $mdProps = (Get-Item $mdKey).Property
        foreach ($prop in $mdProps) {
            $val = (Get-ItemProperty $mdKey).$prop
            $hex = ($val | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
            $out += "  $prop = $hex"
        }
    } else {
        $out += "  MountedDevices NOT FOUND!"
    }

    $out += "=== SERVICES RT640X64 ==="
    $rtKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\rt640x64"
    if (Test-Path $rtKey) {
        $rt = Get-ItemProperty $rtKey
        $out += "  Start = $($rt.Start)"
        $out += "  Type = $($rt.Type)"
        $out += "  ErrorControl = $($rt.ErrorControl)"
        $out += "  Group = $($rt.Group)"
        $out += "  ImagePath = $($rt.ImagePath)"
        $out += "  BootFlags = $($rt.BootFlags)"
        $pKey = "$rtKey\Parameters"
        if (Test-Path $pKey) {
            $out += "  Parameters exists: " + ((Get-ItemProperty $pKey) | Out-String)
        }
    } else {
        $out += "  rt640x64 NOT FOUND!"
    }

    $out += "=== CDDB Realtek ==="
    $cddbKey = "HKLM:\OFFLINE_SYS\ControlSet001\Control\CriticalDeviceDatabase"
    if (Test-Path $cddbKey) {
        $cddb = Get-ChildItem $cddbKey | Where-Object { $_.PSChildName -like "*10ec*" }
        foreach ($c in $cddb) {
            $out += "  $($c.PSChildName) -> Service=$((Get-ItemProperty $c.PSPath).Service)"
        }
    }

    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg unload HKLM\OFFLINE_SYS
}

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_dump.txt" -Encoding utf8
