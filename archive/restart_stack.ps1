# Restart the CobDiskless stack on the freshly built binary and ensure PC-03
# points at the UEFI image. Must run elevated: arming mounts VHDs, and a
# non-elevated Stop-Process on a running cobdiskless_app returns Access denied.
#
# Launching rule that matters: the server must get its OWN console. A plain
# Start-Process reuses the caller's console, so when this wrapper exits the
# console closes and Windows posts CTRL_CLOSE_EVENT to the server, which has
# no handler and dies. `start` allocates a fresh console owned by the server.
$ErrorActionPreference = 'Continue'
$root   = 'C:\Users\Admin\Desktop\cobdiskless'
$build  = Join-Path $root 'build'
$stations = Join-Path $build 'stations.json'
$exe    = Join-Path $build 'bin\cobdiskless_app.exe'
$log    = Join-Path $build 'restart_log.txt'

function Log([string]$m) {
    Add-Content -Path $log -Value ("{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)
}
Set-Content -Path $log -Value ("{0}  === restart begin ===" -f (Get-Date -Format 'HH:mm:ss'))
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log "elevated: $isAdmin"
if (-not $isAdmin) { Log "FAILED: not elevated"; exit 10 }

# --- 1. stop any running stack --------------------------------------------
Get-Process -Name cobdiskless_app -ErrorAction SilentlyContinue | ForEach-Object {
    Log "stopping PID $($_.Id) (started $($_.StartTime))"
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}
$still = $null
foreach ($n in 1..40) {
    Start-Sleep -Milliseconds 250
    $still = Get-Process -Name cobdiskless_app -ErrorAction SilentlyContinue
    if (-not $still) { break }
}
if ($still) { Log "FAILED: old process still running"; exit 1 }
Log "no cobdiskless_app running"

# --- 2. leftover VHD mounts from the previous run --------------------------
$letters = @()
foreach ($d in @('I','J','K','L','M','N')) {
    if (Test-Path "${d}:") {
        try { if ((Get-Volume -DriveLetter $d -ErrorAction Stop).DriveType -eq 'Fixed') { $letters += $d } } catch {}
    }
}
Log "possible leftover mounted image drives: $(if ($letters) { $letters -join ',' } else { 'none' })"

# --- 3. ensure PC-03 -> w11eufi / uefi (idempotent) ------------------------
$lines = [System.IO.File]::ReadAllLines($stations)
$nameIdx = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '"name":\s*"PC-03"') { $nameIdx = $i; break }
}
if ($nameIdx -lt 0) { Log "FAILED: PC-03 not found in stations.json"; exit 3 }

$imgLine = $bmLine = ''
for ($j = $nameIdx; $j -lt [Math]::Min($nameIdx + 12, $lines.Count); $j++) {
    if ($lines[$j] -match '^\s*"image":')       { $imgLine = $j }
    if ($lines[$j] -match '^\s*"boot_mode":')   { $bmLine  = $j }
}
if ($imgLine -eq '' -or $bmLine -eq '') { Log "FAILED: image/boot_mode not found for PC-03"; exit 4 }

$wantImg = 'w11eufi'; $wantBm = 'uefi'
$curImg = ([regex]::Match($lines[$imgLine], '"image":\s*"([^"]*)"')).Groups[1].Value
$curBm  = ([regex]::Match($lines[$bmLine],  '"boot_mode":\s*"([^"]*)"')).Groups[1].Value
Log "PC-03 now: image=$curImg boot_mode=$curBm"
if ($curImg -ne $wantImg) { $lines[$imgLine] = $lines[$imgLine] -replace "`"$curImg`"", "`"$wantImg`"" }
if ($curBm  -ne $wantBm)  { $lines[$bmLine]  = $lines[$bmLine]  -replace "`"$curBm`"",  "`"$wantBm`""  }
if ($curImg -ne $wantImg -or $curBm -ne $wantBm) {
    [System.IO.File]::WriteAllLines($stations, $lines)
    Log "PC-03 -> image=$wantImg, boot_mode=$wantBm"
} else {
    Log "PC-03 already image=$wantImg boot_mode=$wantBm (unchanged)"
}

# --- 4. relaunch with a private console ------------------------------------
if (-not (Test-Path $exe)) { Log "FAILED: $exe not found"; exit 5 }
Log "launching via 'start' (fresh console, detached): $exe"
$inner = '/c start "CobDiskless Stack" /min "' + $exe + '" --start'
Start-Process -FilePath 'cmd.exe' -ArgumentList $inner -WorkingDirectory $build -WindowStyle Minimized

# --- 5. verify immediately, then again after 30 s ---------------------------
function Probe([string]$phase) {
    $p = Get-Process -Name cobdiskless_app -ErrorAction SilentlyContinue
    $tcp = (Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
            Where-Object { $_.LocalPort -in 3260,18080 } |
            ForEach-Object { $_.LocalPort } | Sort-Object -Unique) -join ','
    $udp = (Get-NetUDPEndpoint -ErrorAction SilentlyContinue |
            Where-Object { $_.LocalPort -in 67,69 } |
            ForEach-Object { $_.LocalPort } | Sort-Object -Unique) -join ','
    Log ("{0}: pid=[{1}] tcp=[{2}] udp=[{3}]" -f $phase,
        ($(if ($p) { ($p | ForEach-Object Id) -join ',' } else { 'GONE' })), $tcp, $udp)
    return ($null -ne $p -and $tcp -match '18080' -and $tcp -match '3260' -and $udp -match '67')
}
Start-Sleep -Seconds 6
$ok1 = Probe 't+6s'
Start-Sleep -Seconds 30
$ok2 = Probe 't+36s'
Log $(if ($ok1 -and $ok2) { "OK: server stayed up on both checks" } else { "FAILED: server not healthy (early=$ok1 late=$ok2)" })
Log "=== restart end ==="
