# CobDiskless - Automated VHD Remediation Script
# Runs elevated to strip WOF/CompactOS from boot path & inject PnP stabilization keys

# Set this to the base VHD you want to remediate before running.
$vhdPath = ".\images\csmw10.VHD"
$logPath = Join-Path $PSScriptRoot "fix_vhd_payload.log"

Start-Transcript -Path $logPath -Force

Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "CobDiskless Automated VHD Remediation" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "[*] Target VHD: $vhdPath"

if (-not (Test-Path $vhdPath)) {
    Write-Error "VHD path $vhdPath does not exist!"
    Stop-Transcript
    exit 1
}

# 1. Stop any running cobdiskless_app process that holds a lock on the base VHD
$wasRunning = $false
$cobProc = Get-Process cobdiskless_app -ErrorAction SilentlyContinue
if ($cobProc) {
    Write-Host "[1] Stopping running cobdiskless_app (PID: $($cobProc.Id)) to release VHD locks..." -ForegroundColor Yellow
    $wasRunning = $true
    $procPath = $cobProc.Path
    Stop-Process -Id $cobProc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
} else {
    Write-Host "[1] No running cobdiskless_app process detected."
}

# 2. Clear Read-Only attribute on VHD file
Write-Host "[2] Ensuring VHD file is writable (clearing Read-Only attribute)..."
attrib -r "$vhdPath"
(Get-Item $vhdPath).IsReadOnly = $false

# Dismount if previously mounted
Dismount-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue | Out-Null
Start-Sleep -Seconds 1

# 3. Mount VHD
Write-Host "[3] Mounting VHD as Read-Write..."
$mountedImage = Mount-DiskImage -ImagePath $vhdPath -StorageType VHD -Access ReadWrite -PassThru
Start-Sleep -Seconds 2

$disk = Get-DiskImage -ImagePath $vhdPath | Get-Disk
if (-not $disk) {
    Write-Error "Failed to retrieve mounted disk object!"
    Stop-Transcript
    exit 1
}
Write-Host "[+] Mounted as Disk Number: $($disk.Number)"

# 4. Locate Main OS Partition and Drive Letter
Write-Host "[4] Locating Windows OS partition..."
$osPart = $null
$osDrive = $null

$partitions = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Type -ne 'Reserved' }

# Assign drive letters to any unassigned partition on this disk
$usedLetters = (Get-Volume | Where-Object DriveLetter).DriveLetter
$availLetters = [char[]]([char]'Z'..[char]'D') | Where-Object { $_ -notin $usedLetters }
$letterIdx = 0

foreach ($p in $partitions) {
    if (-not $p.DriveLetter -and $letterIdx -lt $availLetters.Count) {
        $newLetter = $availLetters[$letterIdx++]
        Write-Host "    Assigning drive letter $newLetter`: to Partition $($p.PartitionNumber)..."
        Set-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -NewDriveLetter $newLetter -ErrorAction SilentlyContinue
    }
}

Start-Sleep -Seconds 1

# Refresh partition list to find the one with \Windows\System32\config\SYSTEM
$partitions = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.DriveLetter }
foreach ($p in $partitions) {
    $testDrive = "$($p.DriveLetter):"
    if (Test-Path "$testDrive\Windows\System32\config\SYSTEM") {
        $osDrive = $testDrive
        $osPart = $p
        Write-Host "[+] Found Windows OS on drive: $osDrive (Partition $($p.PartitionNumber))" -ForegroundColor Green
        break
    }
}

if (-not $osDrive) {
    Write-Error "Could not locate Windows partition containing \Windows\System32\config\SYSTEM!"
    Dismount-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue | Out-Null
    Stop-Transcript
    exit 1
}

# 5. Completely Strip CompactOS / WOF Compression from Boot Path
Write-Host "[5] Completely stripping CompactOS / WOF compression from boot-critical path..." -ForegroundColor Yellow

# compact.exe accepts only /EXE:<algorithm> (XPRESS4K, XPRESS8K, XPRESS16K,
# LZX), so "/EXE:none" is a syntax error: it prints usage and changes nothing,
# which is what every /EXE:none call below used to do while looking like it
# worked. WOF / CompactOS binaries are "EXE compressed" and plain /U only
# touches NTFS-compressed files, so every target is run both ways -- /EXE for
# the WOF case, bare /U for the NTFS one. Returns how many of those
# invocations failed, so the caller can report them instead of dropping them.
function Invoke-CompactDecompress {
    param([string[]]$Arguments)

    $failures = 0
    foreach ($swap in @(@('/u', '/f', '/exe'), @('/u', '/f'))) {
        $label   = $swap -join ' '
        $argList = $swap + $Arguments
        # No 2>&1: compact.exe writes its usage and results to stdout, and
        # redirecting the error stream here would make PowerShell wrap its
        # stderr in a NativeCommandError stack block instead of letting the
        # reason through as plain text. The exit code below is what is checked.
        $out     = (& compact.exe @argList | Out-String)

        if ($out -match 'Displays or alters the compression') {
            Write-Host "       [!] $label rejected (usage printed) for $($Arguments -join ' ')" -ForegroundColor Red
            $failures++
        }
        elseif ($LASTEXITCODE -ne 0) {
            Write-Host "       [!] $label exited with code ${LASTEXITCODE} for $($Arguments -join ' ')" -ForegroundColor Red
            $trimmed = $out.Trim()
            if ($trimmed) { Write-Host ('          ' + $trimmed) }
            $failures++
        }
        else {
            Write-Host "       [$label] ok"
        }
    }
    return $failures
}

$compactFailures = 0
$bootPathTargets = @(
    "$osDrive\Windows\Boot\Resources\*.*",
    "$osDrive\Windows\Boot\Resources\en-US\*.*",
    "$osDrive\Windows\System32\bootres.dll",
    "$osDrive\Windows\System32\bootvid.dll"
)
foreach ($t in $bootPathTargets) {
    if ($t -notmatch '\*' -and -not (Test-Path -LiteralPath $t)) {
        Write-Host "    -> (absent, skipped) $t"
        continue
    }
    Write-Host "    -> Decompressing $t"
    $compactFailures += Invoke-CompactDecompress -Arguments @($t)
}

Write-Host "    -> Decompressing drivers under $osDrive\Windows\System32\drivers\"
$compactFailures += Invoke-CompactDecompress -Arguments @("/s:$osDrive\Windows\System32\drivers", '*.*')

# Extra hardening: Re-write bootres.dll.mui cleanly to eliminate any leftover Alternate Data Streams (ADS)
$bootresMui = "$osDrive\Windows\Boot\Resources\en-US\bootres.dll.mui"
if (Test-Path $bootresMui) {
    Write-Host "    -> Inspecting alternate streams on $bootresMui..."
    $streams = Get-Item $bootresMui -Stream * -ErrorAction SilentlyContinue
    foreach ($s in $streams) {
        Write-Host "       Stream found: $($s.Stream)"
        if ($s.Stream -and $s.Stream -ne ':$DATA') {
            Write-Host "       Removing stream: $($s.Stream)"
            Remove-Item "$bootresMui`:$($s.Stream)" -Force -ErrorAction SilentlyContinue
        }
    }
    # Read raw bytes and re-save to guarantee normal uncompressed clusters
    $bytes = [System.IO.File]::ReadAllBytes($bootresMui)
    [System.IO.File]::WriteAllBytes($bootresMui, $bytes)
    $compactFailures += Invoke-CompactDecompress -Arguments @($bootresMui)

    # Confirm the result rather than assuming the calls above worked.
    $muiState = (& compact.exe "$bootresMui" | Out-String)
    if ($muiState -match '1 are compressed') {
        Write-Host "    -> bootres.dll.mui is STILL COMPRESSED after decompression." -ForegroundColor Red
        $compactFailures++
    }
    else {
        Write-Host "    -> bootres.dll.mui verified uncompressed (Length: $($bytes.Length) bytes)." -ForegroundColor Green
    }
}

if ($compactFailures -gt 0) {
    Write-Host "[!] $compactFailures compact.exe invocation(s) failed -- see the messages above." -ForegroundColor Red
}

# 6. Inject Hardware Stabilization Keys into Offline Registry Hive
Write-Host "[6] Injecting hardware stabilization keys into offline SYSTEM registry hive..." -ForegroundColor Yellow
$sysHive = "$osDrive\Windows\System32\config\SYSTEM"

# Ensure hive is not locked
& reg.exe unload HKLM\OFFLINE_SYSTEM 2>$null
& reg.exe load HKLM\OFFLINE_SYSTEM "$sysHive"

if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to load offline registry hive at $sysHive into HKLM\OFFLINE_SYSTEM!"
} else {
    Write-Host "[+] Hive loaded into HKLM\OFFLINE_SYSTEM" -ForegroundColor Green

    # Enumerate ControlSets
    $controlSets = @("ControlSet001")
    if (Test-Path "HKLM:\OFFLINE_SYSTEM\ControlSet002") {
        $controlSets += "ControlSet002"
    }

    foreach ($cs in $controlSets) {
        Write-Host "    Processing $cs..."

        # rt640x64 Parameters
        $rtKey = "HKLM:\OFFLINE_SYSTEM\$cs\Services\rt640x64\Parameters"
        if (-not (Test-Path $rtKey)) {
            New-Item -Path $rtKey -Force | Out-Null
        }
        Set-ItemProperty -Path $rtKey -Name "WaitAutoNegComplete" -Value 1 -Type DWord
        Set-ItemProperty -Path $rtKey -Name "LinkDownDelay" -Value 10 -Type DWord
        Set-ItemProperty -Path $rtKey -Name "SkipPhyReset" -Value 1 -Type DWord
        Write-Host "       Set rt640x64\Parameters: WaitAutoNegComplete=1, LinkDownDelay=10, SkipPhyReset=1"

        # Also set on generic rt68cx64, e1dexpress, e2fexpress if keys exist
        foreach ($drv in @("rt68cx64", "e1dexpress", "e2fexpress")) {
            $drvKey = "HKLM:\OFFLINE_SYSTEM\$cs\Services\$drv\Parameters"
            if (Test-Path "HKLM:\OFFLINE_SYSTEM\$cs\Services\$drv") {
                if (-not (Test-Path $drvKey)) { New-Item -Path $drvKey -Force | Out-Null }
                Set-ItemProperty -Path $drvKey -Name "WaitAutoNegComplete" -Value 1 -Type DWord
                Set-ItemProperty -Path $drvKey -Name "LinkDownDelay" -Value 10 -Type DWord
                Set-ItemProperty -Path $drvKey -Name "SkipPhyReset" -Value 1 -Type DWord
                Write-Host "       Set $drv\Parameters: WaitAutoNegComplete=1, LinkDownDelay=10, SkipPhyReset=1"
            }
        }

        # Tcpip Parameters: DisableDHCPMediaSense = 1
        $tcpKey = "HKLM:\OFFLINE_SYSTEM\$cs\Services\Tcpip\Parameters"
        if (-not (Test-Path $tcpKey)) {
            New-Item -Path $tcpKey -Force | Out-Null
        }
        Set-ItemProperty -Path $tcpKey -Name "DisableDHCPMediaSense" -Value 1 -Type DWord
        Write-Host "       Set Tcpip\Parameters: DisableDHCPMediaSense=1"

        # NIC Network Class Properties: *EEE=0, *GreenEthernet=0, MSISupported=0
        $classKey = "HKLM:\OFFLINE_SYSTEM\$cs\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}"
        if (Test-Path $classKey) {
            Get-ChildItem -Path $classKey | Where-Object { $_.PSChildName -match '^\d{4}$' } | ForEach-Object {
                $instKey = $_.PSPath
                Set-ItemProperty -Path $instKey -Name "*EEE" -Value "0" -Type String -ErrorAction SilentlyContinue
                Set-ItemProperty -Path $instKey -Name "*GreenEthernet" -Value "0" -Type String -ErrorAction SilentlyContinue
                Set-ItemProperty -Path $instKey -Name "GigaLite" -Value "0" -Type String -ErrorAction SilentlyContinue
                Set-ItemProperty -Path $instKey -Name "PowerSavingMode" -Value "0" -Type String -ErrorAction SilentlyContinue
                Set-ItemProperty -Path $instKey -Name "AutoDisablePhy" -Value "0" -Type String -ErrorAction SilentlyContinue
                Set-ItemProperty -Path $instKey -Name "WaitAutoNegComplete" -Value "1" -Type String -ErrorAction SilentlyContinue
                Set-ItemProperty -Path $instKey -Name "SkipPhyReset" -Value 1 -Type DWord -ErrorAction SilentlyContinue
                
                # Interrupt Management\MessageSignaledInterruptProperties: MSISupported = 0
                $msiKey = "$instKey\Interrupt Management\MessageSignaledInterruptProperties"
                if (-not (Test-Path $msiKey)) {
                    New-Item -Path $msiKey -Force -ErrorAction SilentlyContinue | Out-Null
                }
                if (Test-Path $msiKey) {
                    Set-ItemProperty -Path $msiKey -Name "MSISupported" -Value 0 -Type DWord -ErrorAction SilentlyContinue
                }
            }
            Write-Host "       Updated Network Class {4d36e972-...} adapters: *EEE=0, *GreenEthernet=0, MSISupported=0"
        }
    }

    # 7. Clean Unload Hive
    Write-Host "[7] Unloading hive HKLM\OFFLINE_SYSTEM..."
    [GC]::Collect()
    Start-Sleep -Milliseconds 500
    & reg.exe unload HKLM\OFFLINE_SYSTEM
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[+] Offline hive unloaded successfully." -ForegroundColor Green
    } else {
        Write-Warning "First unload attempt returned $LASTEXITCODE; retrying..."
        Start-Sleep -Seconds 1
        & reg.exe unload HKLM\OFFLINE_SYSTEM
    }
}

# 8. Dismount VHD
Write-Host "[8] Dismounting VHD image..."
Dismount-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue | Out-Null
Start-Sleep -Seconds 1
Write-Host "[+] VHD dismounted cleanly." -ForegroundColor Green

# 9. Restore Read-Only attribute on VHD file
Write-Host "[9] Restamping VHD file as Read-Only..."
attrib +r "$vhdPath"

Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "REMEDIATION COMPLETE" -ForegroundColor Green
Write-Host "=========================================================" -ForegroundColor Cyan

Stop-Transcript
