# Read-only inspection of the armed boot VHD (Rule 1: no pre-arming, inspect only).
# Step 1: cob_armer inspect (needs SeBackupPrivilege -> elevated) = armed-state report.
# Step 2: attach the VHD (diskpart readonly) and copy out boot-chain drivers + SYSTEM
#         hive + BCD for offline analysis. Nothing in the VHD is modified.
$ErrorActionPreference = "Continue"
$dest  = "C:\Users\Admin\AppData\Local\Temp\opencode\guestdrv"
$vhd   = "G:\G\csmw10.VHD"
$armer = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\cob_armer.exe"

Start-Transcript -Path "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\extract_guest_drivers3.log" -Force
try { New-Item -ItemType Directory -Path $dest -Force | Out-Null } catch {}

Write-Host "=== STEP 1: cob_armer inspect (read-only, elevated) ==="
& $armer inspect $vhd *> "$dest\armer_inspect_post4.txt"
Write-Host ("cob_armer exit = " + $LASTEXITCODE)
if (Test-Path "$dest\armer_inspect_post4.txt") {
    Write-Host ("inspect output bytes: " + (Get-Item "$dest\armer_inspect_post4.txt").Length)
}

# Did cob_armer leave the VHD attached? If a Windows volume is already up, reuse it.
$winVol = $null
foreach ($l in @("Y", "W", "Q", "I", "X", "V", "U", "T", "S", "R", "P", "N", "M")) {
    if (Test-Path "${l}:\Windows\System32\config\SYSTEM") { $winVol = "${l}:"; break }
}
Write-Host ("pre-existing mounted windows volume: " + $(if ($winVol) { $winVol } else { "none" }))

$weAttached = $false
if (-not $winVol) {
    Write-Host "=== STEP 2: diskpart attach readonly ==="
    $dp = @"
select vdisk file="$vhd"
attach vdisk readonly
exit
"@
    $dp | diskpart
    Start-Sleep -Seconds 3

    # enumerate volumes; pick the one carrying Windows
    $vols = (mountvol) -split "`r?`n" | Where-Object { $_ -match 'Volume\{' }
    Write-Host ("volumes found: " + $vols.Count)
    $mnt = "$dest\mnt"
    foreach ($v in $vols) {
        $g = $v.Trim()
        try {
            if (Test-Path $mnt) { mountvol $mnt /d 2>$null }
            mountvol $mnt $g 2>$null
            if (Test-Path "$mnt\Windows\System32\config\SYSTEM") {
                $winVol = $mnt
                Write-Host ("windows volume mounted at $mnt -> " + $g)
                break
            }
            mountvol $mnt /d 2>$null
        } catch {}
    }
    $weAttached = $true
}

if (-not $winVol) {
    Write-Host "FATAL: could not locate a Windows volume on the VHD"
    if ($weAttached) { $dp2 = "select vdisk file=`"$vhd`"`ndetach vdisk`nexit"; $dp2 | diskpart }
    Stop-Transcript; exit 11
}

Write-Host "=== STEP 3: driver presence + copy ==="
$drivers = @("msiscsi.sys","storport.sys","tcpip.sys","ndis.sys","E1G6032E.sys",
             "e1i63x64.sys","e1i68x64.sys","Rtnic64.sys","storahci.sys","disk.sys",
             "partmgr.sys","vdrvroot.sys","volmgr.sys","acpiex.sys","e1g3.sys",
             "rt640x64.sys","pciwhea.sys","spaceport.sys","volmgrx.sys","mountmgr.sys",
             "kd","netwtw10.sys")
foreach ($d in $drivers) {
    $src = Join-Path $winVol "Windows\System32\drivers\$d"
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $dest $d) -Force -ErrorAction SilentlyContinue
        $h = (Get-FileHash (Join-Path $dest $d) -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
        Write-Host ("  PRESENT  {0,-16} sha256={1}" -f $d, $h)
    } else {
        Write-Host ("  missing  {0,-16}" -f $d)
    }
}

Write-Host "=== STEP 3b: drivers dir full listing (staged NIC binaries) ==="
Get-ChildItem (Join-Path $winVol "Windows\System32\drivers") -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(e1|rt|E1|Rt|net|E1G)' } |
    ForEach-Object { Write-Host ("  {0,-28} {1,10} {2}" -f $_.Name, $_.Length, $_.LastWriteTime) }

Write-Host "=== STEP 3c: DriverStore e1g3/realtek packages ==="
Get-ChildItem (Join-Path $winVol "Windows\System32\DriverStore\FileRepository") -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'e1g3|e1g6|net1ic|netrtl|realtek' } |
    ForEach-Object { Write-Host ("  " + $_.Name) }

Write-Host "=== STEP 4: hive + BCD copy ==="
Copy-Item (Join-Path $winVol "Windows\System32\config\SYSTEM") "$dest\SYSTEM_hive" -Force
Write-Host ("SYSTEM_hive bytes: " + (Get-Item "$dest\SYSTEM_hive").Length)
foreach ($rel in @("Boot\BCD", "Windows\Boot\BCD", "Boot\BCD.BAK")) {
    $p = Join-Path $winVol $rel
    if (Test-Path $p) {
        Copy-Item $p "$dest\BCD_copy" -Force
        Write-Host ("BCD copied from $rel bytes: " + (Get-Item "$dest\BCD_copy").Length)
        break
    }
}

Write-Host "=== STEP 5: cleanup ==="
if ($winVol -like "*\mnt") {
    try { mountvol $winVol /d 2>$null; Write-Host "unmounted mount dir" } catch {}
}
if ($weAttached) {
    $dp3 = "select vdisk file=`"$vhd`"`ndetach vdisk`nexit"
    $dp3 | diskpart
    Write-Host "detached vdisk"
} else {
    Write-Host "vhd was not attached by us; leaving as-is"
}

Write-Host "[DONE]"
Stop-Transcript
