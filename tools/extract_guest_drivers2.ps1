# Read-only inspection of the armed boot VHD (Rule 1: no pre-arming, inspect only).
# Mounts G:\G\csmw10.VHD read-only, copies out boot-chain drivers + SYSTEM hive + BCD
# for offline analysis, then dismounts. Nothing in the VHD is modified.
$ErrorActionPreference = "Continue"
$dest = "C:\Users\Admin\AppData\Local\Temp\opencode\guestdrv"
$vhd  = "G:\G\csmw10.VHD"

Start-Transcript -Path "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\extract_guest_drivers2.log" -Force

try { New-Item -ItemType Directory -Path $dest -Force | Out-Null } catch {}

Write-Host "[1] VHD info:"
$img = Get-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue
if (-not $img) { Write-Host "    (not currently attached)"; } else { Write-Host "    attached=$($img.Attached)" }

$mountedByUs = $false
if (-not ($img -and $img.Attached)) {
    Write-Host "[2] Mounting read-only..."
    try {
        $img = Mount-VHD -Path $vhd -ReadOnly -PassThru -ErrorAction Stop
        $mountedByUs = $true
        Start-Sleep -Seconds 2
    } catch {
        Write-Host "[2] RO mount FAILED: $($_.Exception.Message)"
        Write-Host "    -> trying plain Mount-DiskImage..."
        try {
            $img = Mount-DiskImage -ImagePath $vhd -PassThru -ErrorAction Stop
            $mountedByUs = $true
            Start-Sleep -Seconds 2
        } catch {
            Write-Host "[2] fallback mount FAILED too: $($_.Exception.Message)"
            Stop-Transcript; exit 10
        }
    }
} else {
    Write-Host "[2] VHD already attached (used existing mount)"
}

$disk = $img | Get-Disk
Write-Host "    disk number: $($disk.Number)  style: $($disk.PartitionStyle)"

$letters = @()
foreach ($p in ($disk | Get-Partition)) {
    if (-not $p.DriveLetter) {
        try { Add-PartitionAccessPath -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber -AssignDriveLetter -ErrorAction Stop } catch {}
        $p = Get-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber
    }
    Write-Host "    partition $($p.PartitionNumber): letter=$($p.DriveLetter) size=$($p.Size) type=$($p.Type)"
    if ($p.DriveLetter) { $letters += "$($p.DriveLetter):" }
}

$sysVol = $null
foreach ($l in $letters) {
    if (Test-Path "$l\Windows\System32\config\SYSTEM") { $sysVol = $l; break }
}
if (-not $sysVol) {
    Write-Host "[3] NO volume with Windows found; letters: $($letters -join ', ')"
    if ($mountedByUs) { Dismount-VHD -Path $vhd -ErrorAction SilentlyContinue }
    Stop-Transcript; exit 11
}
Write-Host "[3] Windows volume: $sysVol"

Write-Host "[4] Driver presence + copy:"
$drivers = @("msiscsi.sys","storport.sys","tcpip.sys","ndis.sys","e1000.sys","e1000b.sys",
             "storahci.sys","disk.sys","partmgr.sys","vdrvroot.sys","volmgr.sys","acpiex.sys",
             "pnic.sys","rtl8139.sys")
foreach ($d in $drivers) {
    $src = "$sysVol\Windows\System32\drivers\$d"
    if (Test-Path $src) {
        Copy-Item $src "$dest\$d" -Force -ErrorAction SilentlyContinue
        $h = (Get-FileHash "$dest\$d" -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
        Write-Host ("    PRESENT  {0,-14} sha256={1}" -f $d, $h)
    } else {
        Write-Host ("    MISSING  {0,-14}" -f $d)
    }
}

Write-Host "[5] Hive + BCD copy:"
Copy-Item "$sysVol\Windows\System32\config\SYSTEM" "$dest\SYSTEM_hive" -Force
Write-Host ("    SYSTEM_hive bytes: " + (Get-Item "$dest\SYSTEM_hive").Length)
foreach ($b in @("$sysVol\Boot\BCD", "$sysVol\Windows\Boot\BCD")) {
    if (Test-Path $b) {
        Copy-Item $b "$dest\BCD_copy" -Force
        Write-Host ("    BCD copied from $b bytes: " + (Get-Item "$dest\BCD_copy").Length)
        break
    }
}

Write-Host "[6] Dismounting..."
if ($mountedByUs) {
    try { Dismount-VHD -Path $vhd -ErrorAction Stop; Write-Host "    dismounted OK" }
    catch { Write-Host "    dismount FAILED: $($_.Exception.Message)" }
}

Write-Host "[DONE]"
Stop-Transcript
