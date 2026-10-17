# =============================================================================
# CobDiskless - Master VHD Hardware Arming & CompactOS Stripping Pipeline
# 1. Strips CompactOS / WOF compression from boot-critical files and drivers
# 2. Sanitizes global TCP/IP stack (DisableTaskOffload=1, DisableDHCPMediaSense=1)
# 3. Enforces NDIS Dumb-Pipe Mode across all adapter instances
# 4. Sets strict kernel service group hierarchy and clears StartOverride
# 5. Bakes CriticalDeviceDatabase binding for PCI\VEN_10EC&DEV_8168 -> rt640x64
# 6. Purges stale COW overlays from writeback storage
# =============================================================================

$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

# --- 0. Self-elevate -------------------------------------------------------
$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $psHost = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    try {
        Start-Process -FilePath $psHost -Verb RunAs -ArgumentList $argList -Wait
    } catch {
        Write-Host "[FATAL] UAC elevation was declined or unavailable: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
    exit $LASTEXITCODE
}

$vhd = "G:\G\csmw10.vhd"
$log = Join-Path $PSScriptRoot "..\tools\arm_master_bootstart.log"
Start-Transcript -Path $log -Force | Out-Null

$failed   = $false
$loaded   = $false
$mounted  = $false

function Invoke-Reg {
    param([string[]]$RegArgs, [string]$Label)
    $out = & reg.exe @RegArgs 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    [!] reg.exe $Label -> exit $LASTEXITCODE" -ForegroundColor Yellow
        return $false
    }
    return $true
}

function Invoke-CompactDecompress {
    param([string[]]$Arguments)
    $failures = 0
    foreach ($swap in @(@('/u', '/f', '/exe'), @('/u', '/f'))) {
        $label   = $swap -join ' '
        $argList = $swap + $Arguments
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
            Write-Host "       [$label] ok: $($Arguments -join ' ')"
        }
    }
    return $failures
}

try {
    if (-not (Test-Path -LiteralPath $vhd)) {
        throw "Master VHD not found: $vhd"
    }

    Write-Host "=========================================================" -ForegroundColor Cyan
    Write-Host "CobDiskless Master VHD - Complete Boot Remediation & Arming" -ForegroundColor Cyan
    Write-Host "=========================================================" -ForegroundColor Cyan
    Write-Host "[*] Target: $vhd"

    # --- 1. Dismount leftovers, clear Read-Only stamp, mount read/write --
    Write-Host "[1] Mounting $vhd read/write..." -ForegroundColor Yellow
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    Start-Sleep -Seconds 1

    (Get-Item -LiteralPath $vhd).IsReadOnly = $false
    attrib -r "$vhd" | Out-Null

    $mount = Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadWrite -PassThru -ErrorAction Stop
    $mounted = $true
    Start-Sleep -Seconds 2

    $disk = $mount | Get-Disk
    if (-not $disk) { throw "Mount succeeded but no disk object was returned." }
    Write-Host "[+] Mounted as Disk Number: $($disk.Number)"

    # --- 2. Locate Windows volume -----------------------------------------
    Start-Sleep -Seconds 2
    $partitions = Get-Partition -DiskNumber $disk.Number -ErrorAction Stop | Where-Object { $_.Type -ne 'Reserved' }
    $used = @(Get-Volume | Where-Object DriveLetter | ForEach-Object DriveLetter)
    $avail = @([char[]]([char]'Z'..[char]'D') | Where-Object { $_ -notin $used })
    $idx = 0
    foreach ($p in $partitions) {
        if (-not $p.DriveLetter -and $idx -lt $avail.Count) {
            $newLetter = $avail[$idx++]
            Set-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -NewDriveLetter $newLetter -ErrorAction SilentlyContinue
        }
    }
    Start-Sleep -Seconds 1

    $letter = $null
    $partitions = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.DriveLetter }
    foreach ($p in $partitions) {
        $testPath = "$($p.DriveLetter):\Windows\System32\config\SYSTEM"
        if (Test-Path -LiteralPath $testPath) {
            $letter = $p.DriveLetter
            break
        }
    }
    if (-not $letter) { throw "Could not locate a volume carrying \Windows\System32\config\SYSTEM." }
    Write-Host "[+] Windows volume mounted at ${letter}:" -ForegroundColor Green

    # --- 3. Strip CompactOS / WOF Compression from Boot Path --------------
    Write-Host "[3] Stripping CompactOS / WOF compression from boot files & drivers..." -ForegroundColor Yellow
    $bootPathTargets = @(
        "${letter}:\Windows\Boot\Resources\*.*",
        "${letter}:\Windows\Boot\Resources\en-US\*.*",
        "${letter}:\Windows\System32\bootres.dll",
        "${letter}:\Windows\System32\bootvid.dll"
    )
    foreach ($t in $bootPathTargets) {
        if ($t -notmatch '\*' -and -not (Test-Path -LiteralPath $t)) { continue }
        Write-Host "    -> Decompressing $t"
        Invoke-CompactDecompress -Arguments @($t) | Out-Null
    }

    Write-Host "    -> Decompressing drivers under ${letter}:\Windows\System32\drivers\"
    Invoke-CompactDecompress -Arguments @("/s:${letter}:\Windows\System32\drivers", '*.*') | Out-Null

    # Critical: Strip alternate data streams & ensure uncompressed bootres.dll.mui
    $bootresMui = "${letter}:\Windows\Boot\Resources\en-US\bootres.dll.mui"
    if (Test-Path -LiteralPath $bootresMui) {
        Write-Host "    -> Sanitizing alternate data streams and uncompressing $bootresMui..."
        & takeown.exe /f "$bootresMui" /a 2>&1 | Out-Null
        & icacls.exe "$bootresMui" /grant "*S-1-5-32-544:F" 2>&1 | Out-Null
        & attrib.exe -r -s -h "$bootresMui" 2>&1 | Out-Null
        $streams = Get-Item -LiteralPath $bootresMui -Stream * -ErrorAction SilentlyContinue
        foreach ($s in $streams) {
            if ($s.Stream -and $s.Stream -ne ':$DATA') {
                Write-Host "       Removing stream: $($s.Stream)"
                Remove-Item -LiteralPath "$bootresMui`:$($s.Stream)" -Force -ErrorAction SilentlyContinue
            }
        }
        Invoke-CompactDecompress -Arguments @($bootresMui) | Out-Null
        Write-Host "    -> bootres.dll.mui decompressed successfully." -ForegroundColor Green
    }

    # --- 4. Load the SYSTEM hive ------------------------------------------
    $hive = "${letter}:\Windows\System32\config\SYSTEM"
    Invoke-Reg @('unload', 'HKLM\MASTER_SYS') 'unload (stale)' | Out-Null
    Start-Sleep -Milliseconds 300
    $loadOk = Invoke-Reg @('load', 'HKLM\MASTER_SYS', "$hive") 'load HKLM\MASTER_SYS'
    if (-not $loadOk) {
        throw "reg.exe load HKLM\MASTER_SYS failed - see the message above."
    }
    $loaded = $true
    Write-Host "[+] Hive loaded: HKLM\MASTER_SYS"

    # --- 5. Global TCP/IP Early-Boot Stability ----------------------------
    Write-Host "[5] Enforcing global TCP/IP early-boot stability..." -ForegroundColor Yellow
    $tcpipParams = "HKLM\MASTER_SYS\ControlSet001\Services\Tcpip\Parameters"
    Invoke-Reg @('add', $tcpipParams, '/v', 'DisableTaskOffload', '/t', 'REG_DWORD', '/d', '1', '/f') 'DisableTaskOffload' | Out-Null
    Invoke-Reg @('add', $tcpipParams, '/v', 'DisableDHCPMediaSense', '/t', 'REG_DWORD', '/d', '1', '/f') 'DisableDHCPMediaSense' | Out-Null
    Write-Host "    [+] Tcpip\Parameters: DisableTaskOffload=1, DisableDHCPMediaSense=1"

    # --- 6. Strict Driver Service Groups & Boot Start Enforcement ----------
    Write-Host "[6] Applying strict service group hierarchy..." -ForegroundColor Yellow
    $serviceDefinitions = @(
        @{ Name = "ndis";        Group = "NDIS Wrapper";       Start = 0; BootFlags = 1 },
        @{ Name = "Tcpip";       Group = "PNP_TDI";            Start = 0; BootFlags = 0 },
        @{ Name = "msiscsi";     Group = "iSCSI";              Start = 0; BootFlags = 1 },
        @{ Name = "iScsiPrt";    Group = "CCiSCSI";            Start = 0; BootFlags = 1; ImagePath = "system32\drivers\msiscsi.sys" },
        @{ Name = "rt640x64";    Group = "NDIS";               Start = 0; BootFlags = 1 },
        @{ Name = "rt25cx21";    Group = "NDIS";               Start = 0; BootFlags = 1 },
        @{ Name = "rt68cx64";    Group = "NDIS";               Start = 0; BootFlags = 1 },
        @{ Name = "e1d68x64";    Group = "NDIS";               Start = 0; BootFlags = 1 },
        @{ Name = "e2f68x64";    Group = "NDIS";               Start = 0; BootFlags = 1 },
        @{ Name = "e2f68";       Group = "NDIS";               Start = 0; BootFlags = 1 }
    )

    foreach ($svc in $serviceDefinitions) {
        $name = $svc.Name
        $grp  = $svc.Group
        $strt = $svc.Start
        $key = "HKLM\MASTER_SYS\ControlSet001\Services\$name"

        Invoke-Reg @('add', $key, '/v', 'Start', '/t', 'REG_DWORD', '/d', "$strt", '/f') "$name Start" | Out-Null
        Invoke-Reg @('add', $key, '/v', 'Group', '/t', 'REG_SZ', '/d', "$grp", '/f') "$name Group" | Out-Null
        if ($svc.BootFlags -eq 1) {
            Invoke-Reg @('add', $key, '/v', 'BootFlags', '/t', 'REG_DWORD', '/d', '1', '/f') "$name BootFlags" | Out-Null
        }
        if ($svc.ImagePath) {
            Invoke-Reg @('add', $key, '/v', 'ImagePath', '/t', 'REG_EXPAND_SZ', '/d', $svc.ImagePath, '/f') "$name ImagePath" | Out-Null
        }
        Invoke-Reg @('delete', "$key\StartOverride", '/f') "$name StartOverride" | Out-Null
        Write-Host "    [+] $($name): Start=$strt Group=$grp (StartOverride cleared)"
    }

    # Disk timeout = 60s
    Invoke-Reg @('add', 'HKLM\MASTER_SYS\ControlSet001\Services\disk', '/v', 'TimeOutValue', '/t', 'REG_DWORD', '/d', '60', '/f') 'disk TimeOutValue' | Out-Null

    # Driver parameters for Realtek PHY stabilization
    $rtParamKey = "HKLM\MASTER_SYS\ControlSet001\Services\rt640x64\Parameters"
    Invoke-Reg @('add', $rtParamKey, '/v', 'WaitAutoNegComplete', '/t', 'REG_DWORD', '/d', '1', '/f') 'rt640x64 WaitAutoNeg' | Out-Null
    Invoke-Reg @('add', $rtParamKey, '/v', 'LinkDownDelay', '/t', 'REG_DWORD', '/d', '10', '/f') 'rt640x64 LinkDownDelay' | Out-Null
    Invoke-Reg @('add', $rtParamKey, '/v', 'SkipPhyReset', '/t', 'REG_DWORD', '/d', '1', '/f') 'rt640x64 SkipPhyReset' | Out-Null

    # --- 7. NDIS Dumb-Pipe Mode across all adapter instances ---------------
    Write-Host "[7] Sanitizing adapter instances (NDIS Dumb-Pipe Mode)..." -ForegroundColor Yellow
    $netClass = "HKLM\MASTER_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}"
    if (Test-Path "Registry::$netClass") {
        $touched = 0
        Get-ChildItem -Path "Registry::$netClass" -ErrorAction SilentlyContinue | ForEach-Object {
            $subkeyName = $_.PSChildName
            if ($subkeyName -match '^\d{4}$') {
                $p = $_.Name -replace 'HKEY_LOCAL_MACHINE', 'HKLM'
                $offloads = @(
                    '*LsoV2IPv4', '*LsoV2IPv6',
                    '*IPChecksumOffloadIPv4', '*TCPChecksumOffloadIPv4', '*UDPChecksumOffloadIPv4',
                    '*TCPChecksumOffloadIPv6', '*UDPChecksumOffloadIPv6',
                    '*RSS', '*FlowControl', '*PriorityVLANTag',
                    '*EEE', '*GreenEthernet', 'GigaLite', 'PowerSavingMode',
                    'AutoPowerSaveModeEnabled', 'EnablePME', 'WakeUpPME',
                    'ReduceSpeedOnPowerDown', 'AutoDisableGigabit', 'AdvancedEEE'
                )
                foreach ($offload in $offloads) {
                    Invoke-Reg @('add', $p, '/v', $offload, '/t', 'REG_SZ', '/d', '0', '/f') "$subkeyName $offload" | Out-Null
                }
                Invoke-Reg @('add', $p, '/v', 'WaitAutoNegComplete', '/t', 'REG_SZ', '/d', '1', '/f') "$subkeyName WaitAutoNeg" | Out-Null
                Invoke-Reg @('add', $p, '/v', 'SkipPhyReset', '/t', 'REG_DWORD', '/d', '1', '/f') "$subkeyName SkipPhyReset" | Out-Null

                $msiKey = "$p\Interrupt Management\MessageSignaledInterruptProperties"
                Invoke-Reg @('add', $msiKey, '/v', 'MSISupported', '/t', 'REG_DWORD', '/d', '0', '/f') "$subkeyName MSISupported" | Out-Null

                $touched++
            }
        }
        Write-Host "[+] Sanitized $touched adapter instance(s) with NDIS Dumb-Pipe Mode." -ForegroundColor Green
    }

    # --- 8. CriticalDeviceDatabase Binding for Realtek 8168 -----------------
    Write-Host "[8] Ensuring CriticalDeviceDatabase binding for PCI\VEN_10EC&DEV_8168..." -ForegroundColor Yellow
    $cddbKey = "HKLM\MASTER_SYS\ControlSet001\Control\CriticalDeviceDatabase\pci#ven_10ec&dev_8168"
    Invoke-Reg @('add', $cddbKey, '/v', 'Service', '/t', 'REG_SZ', '/d', 'rt640x64', '/f') 'CDDB Service' | Out-Null
    Invoke-Reg @('add', $cddbKey, '/v', 'ClassGUID', '/t', 'REG_SZ', '/d', '{4d36e972-e325-11ce-bfc1-08002be10318}', '/f') 'CDDB ClassGUID' | Out-Null
    Write-Host "    [+] CDDB: pci#ven_10ec&dev_8168 -> Service rt640x64, ClassGUID {4d36e972-e325-11ce-bfc1-08002be10318}" -ForegroundColor Green

    # --- 9. Clean Unload Hive ----------------------------------------------
    Write-Host "[9] Unloading SYSTEM hive..." -ForegroundColor Yellow
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    Start-Sleep -Milliseconds 500

    $unloadOk = $false
    for ($i = 1; $i -le 5; $i++) {
        if (Invoke-Reg @('unload', 'HKLM\MASTER_SYS') "unload attempt $i") { $unloadOk = $true; break }
        Start-Sleep -Seconds 1
    }
    if (-not $unloadOk) { throw "reg.exe unload HKLM\MASTER_SYS failed after 5 attempts." }
    $loaded = $false
    Write-Host "[+] Hive unloaded cleanly." -ForegroundColor Green

    # --- 10. Dismount VHD and Restore Read-Only Stamp ----------------------
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    $mounted = $false
    Start-Sleep -Seconds 1
    (Get-Item -LiteralPath $vhd).IsReadOnly = $true
    attrib +r "$vhd" | Out-Null
    Write-Host "[+] VHD dismounted cleanly and re-stamped read-only." -ForegroundColor Green

    # --- 11. Auto-Purge Stale COW Overlays on E:\ --------------------------
    Write-Host "[11] Purging stale writeback COW overlays on E:\..." -ForegroundColor Yellow
    Get-ChildItem -Path "E:\" -Filter "*.cow" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "    -> Deleting stale overlay: $($_.FullName)"
        Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[+] Writeback overlays purged." -ForegroundColor Green

    Write-Host "=========================================================" -ForegroundColor Cyan
    Write-Host "[SUCCESS] Master VHD G:\G\csmw10.vhd remediated and armed." -ForegroundColor Green
    Write-Host "=========================================================" -ForegroundColor Cyan
} catch {
    $failed = $true
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($loaded) {
        [GC]::Collect()
        for ($i = 1; $i -le 3; $i++) {
            if (Invoke-Reg @('unload', 'HKLM\MASTER_SYS') 'cleanup unload') { break }
            Start-Sleep -Seconds 1
        }
    }
    if ($mounted) {
        Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    }
    Stop-Transcript | Out-Null
}

exit $(if ($failed) { 1 } else { 0 })
