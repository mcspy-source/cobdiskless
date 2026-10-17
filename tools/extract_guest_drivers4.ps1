# Read-only inspection of the armed boot VHD (Rule 1: inspect only, no pre-arming).
# diskpart attach readonly -> locate Windows volume (letter or mountvol) ->
# copy boot-chain drivers + SYSTEM hive + BCD out -> detach. VHD never modified.
$ErrorActionPreference = "Continue"
$dest = "C:\Users\Admin\AppData\Local\Temp\opencode\guestdrv"
$vhd  = "G:\G\csmw10.VHD"

Start-Transcript -Path "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\extract_guest_drivers4.log" -Force
try { New-Item -ItemType Directory -Path $dest -Force | Out-Null } catch {}
New-Item -ItemType Directory -Path "$dest\mnt" -Force | Out-Null | Out-Null

Write-Host "=== diskpart attach readonly ==="
$dp = @"
select vdisk file="$vhd"
attach vdisk readonly
exit
"@
$dp | diskpart
Start-Sleep -Seconds 3

$winVol = $null

# Pass 1: any drive letter exposing the Windows dir
Write-Host "=== letter scan ==="
foreach ($c in [char[]](69..90)) {   # E:..Z:
    $root = "${c}:\"
    if ((Test-Path "$root\Windows\System32\drivers") -and
        (Test-Path "$root\Windows\System32\config\SYSTEM")) {
        $winVol = $root
        Write-Host "  found Windows at $root"
        break
    }
}

# Pass 2: mountvol folder mount for letterless volumes
if (-not $winVol) {
    Write-Host "=== mountvol scan ==="
    $vols = (mountvol) -split "`r?`n" | Where-Object { $_ -match 'Volume\{' }
    foreach ($v in $vols) {
        $g = $v.Trim()
        try {
            mountvol "$dest\mnt" $g 2>$null | Out-Null
            if ((Test-Path "$dest\mnt\Windows\System32\drivers") -and
                (Test-Path "$dest\mnt\Windows\System32\config\SYSTEM")) {
                $winVol = "$dest\mnt\"
                Write-Host "  found Windows via $g"
                break
            }
            mountvol "$dest\mnt" /d 2>$null | Out-Null
        } catch {}
    }
}

if (-not $winVol) {
    Write-Host "FATAL: no Windows volume found"
    "select vdisk file=`"$vhd`"`ndetach vdisk`nexit" | diskpart
    Stop-Transcript; exit 11
}

Write-Host "=== driver presence + copy ==="
$drivers = @("msiscsi.sys","storport.sys","tcpip.sys","ndis.sys","E1G6032E.sys",
             "e1i63x64.sys","e1i68x64.sys","Rtnic64.sys","storahci.sys","disk.sys",
             "partmgr.sys","vdrvroot.sys","volmgr.sys","acpiex.sys","e1g3.sys",
             "rt640x64.sys","spaceport.sys","volmgrx.sys","mountmgr.sys")
foreach ($d in $drivers) {
    $src = "$winVol" + "Windows\System32\drivers\$d"
    if (Test-Path $src) {
        Copy-Item $src "$dest\$d" -Force -ErrorAction SilentlyContinue
        $h = (Get-FileHash "$dest\$d" -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
        Write-Host ("  PRESENT  {0,-16} sha256={1}" -f $d, $h)
    } else {
        Write-Host ("  missing  {0,-16}" -f $d)
    }
}

Write-Host "=== drivers dir NIC-family listing ==="
Get-ChildItem "$($winVol)Windows\System32\drivers" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(e1|rt|E1|Rt|net)' } |
    ForEach-Object { Write-Host ("  {0,-28} {1,10} {2}" -f $_.Name, $_.Length, $_.LastWriteTime) }

Write-Host "=== DriverStore packages (e1g/realtek/net1ic) ==="
Get-ChildItem "$($winVol)Windows\System32\DriverStore\FileRepository" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'e1g3|e1g6|net1ic|netrtl|realtek|rtnet' } |
    ForEach-Object { Write-Host ("  " + $_.Name) }

Write-Host "=== hive + BCD copy ==="
Copy-Item "$($winVol)Windows\System32\config\SYSTEM" "$dest\SYSTEM_hive" -Force
Write-Host ("SYSTEM_hive bytes: " + (Get-Item "$dest\SYSTEM_hive").Length)
foreach ($rel in @("Boot\BCD","Windows\Boot\BCD")) {
    $p = "$($winVol)$rel"
    if (Test-Path $p) {
        Copy-Item $p "$dest\BCD_copy" -Force
        Write-Host ("BCD copied from $rel bytes: " + (Get-Item "$dest\BCD_copy").Length)
        break
    }
}

Write-Host "=== cleanup ==="
if ($winVol -like "*mnt*") { mountvol "$dest\mnt" /d 2>$null | Out-Null }
"select vdisk file=`"$vhd`"`ndetach vdisk`nexit" | diskpart
Write-Host "[DONE]"
Stop-Transcript
