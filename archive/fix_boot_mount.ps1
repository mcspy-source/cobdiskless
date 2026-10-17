# fix_boot_mount.ps1
# Automated offline registry remediation for CobDiskless base VHD
param(
    [string]$VhdPath = "G:\G\csmw10.VHD"
)

$ErrorActionPreference = "Stop"
$LogPath = "C:\Users\Admin\Desktop\cobdiskless\tools\fix_boot_mount.log"
Start-Transcript -Path $LogPath -Force

Write-Host "=== CobDiskless Boot Mount Fix Starting ===" -ForegroundColor Cyan
Write-Host "VHD: $VhdPath"

try {
    # 1. Mount VHD read-write
    Write-Host "[1/6] Mounting VHD as Read-Write..."
    $mountResult = Mount-DiskImage -ImagePath $VhdPath -StorageType VHD -PassThru
    $diskImage = Get-DiskImage -ImagePath $VhdPath
    $disk = Get-Disk -Number $diskImage.Number
    Write-Host "Disk number: $($disk.Number)"

    # Wait for partitions to arrive
    Start-Sleep -Seconds 2

    # 2. Find Partition 2 (Main Windows OS partition)
    Write-Host "[2/6] Locating Windows Partition..."
    $partitions = Get-Partition -DiskNumber $disk.Number
    $osPart = $null
    foreach ($p in $partitions) {
        Write-Host "Partition $($p.PartitionNumber): Size $($p.Size) Type $($p.Type) DriveLetter $($p.DriveLetter)"
        if ($p.Size -gt 5GB) {
            $osPart = $p
        }
    }

    if (-not $osPart) {
        # Fallback to partition 2
        $osPart = Get-Partition -DiskNumber $disk.Number -PartitionNumber 2
    }

    $driveLetter = $osPart.DriveLetter
    if (-not $driveLetter) {
        Write-Host "Assigning temporary drive letter..."
        # Find unused drive letter
        $usedLetters = (Get-Volume).DriveLetter
        $available = [char[]]([int][char]'E'..[int][char]'Z') | Where-Object { $usedLetters -notcontains $_ }
        $driveLetter = $available[0]
        Set-Partition -DiskNumber $disk.Number -PartitionNumber $osPart.PartitionNumber -NewDriveLetter $driveLetter
        Start-Sleep -Seconds 1
    }

    $osDrive = "$($driveLetter):"
    Write-Host "OS Partition assigned at: $osDrive"

    $systemHive = "$osDrive\Windows\System32\config\SYSTEM"
    if (-not (Test-Path $systemHive)) {
        throw "SYSTEM hive not found at $systemHive"
    }

    # 3. Load hive
    Write-Host "[3/6] Loading SYSTEM hive as HKLM\OFFLINE_SYSTEM..."
    & reg.exe load HKLM\OFFLINE_SYSTEM "$systemHive"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to load hive: reg.exe exit code $LASTEXITCODE"
    }

    # 4. Apply Patches
    Write-Host "[4/6] Applying registry configurations..."

    # A. Strip WFP Lightweight Filter (ms_wfplwf) -> Start = 4 (Disabled)
    Write-Host "  -> Setting ms_wfplwf Start = 4 (Disabled)..."
    & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet001\Services\ms_wfplwf" /v Start /t REG_DWORD /d 4 /f
    if ($LASTEXITCODE -ne 0) { Write-Warning "Could not update ms_wfplwf Start" }

    # B. Clear Paging Files
    Write-Host "  -> Clearing PagingFiles..."
    & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet001\Control\Session Manager\Memory Management" /v PagingFiles /t REG_MULTI_SZ /d "" /f
    if ($LASTEXITCODE -ne 0) { Write-Warning "Could not clear PagingFiles" }

    # C. Clear Stale MountedDevices Signatures
    Write-Host "  -> Clearing MountedDevices..."
    # Query before delete
    $mountedCount = (& reg.exe query "HKLM\OFFLINE_SYSTEM\MountedDevices" | Measure-Object).Count
    Write-Host "     Found $mountedCount entries under MountedDevices"
    & reg.exe delete "HKLM\OFFLINE_SYSTEM\MountedDevices" /va /f
    if ($LASTEXITCODE -ne 0) { Write-Warning "Could not clear MountedDevices /va" }

    # D. Enforce TCP/IP Boot Media Sense
    Write-Host "  -> Setting Tcpip\Parameters DisableDHCPMediaSense = 1..."
    & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet001\Services\Tcpip\Parameters" /v DisableDHCPMediaSense /t REG_DWORD /d 1 /f
    if ($LASTEXITCODE -ne 0) { Write-Warning "Could not set DisableDHCPMediaSense" }

    # Also check if ControlSet002 exists and update it too if present
    $cs002Test = & reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet002" 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  -> ControlSet002 detected, synchronizing changes..."
        & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet002\Services\ms_wfplwf" /v Start /t REG_DWORD /d 4 /f
        & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet002\Control\Session Manager\Memory Management" /v PagingFiles /t REG_MULTI_SZ /d "" /f
        & reg.exe add "HKLM\OFFLINE_SYSTEM\ControlSet002\Services\Tcpip\Parameters" /v DisableDHCPMediaSense /t REG_DWORD /d 1 /f
    }

    # Verify changes
    Write-Host "[5/6] Verifying modifications..."
    & reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet001\Services\ms_wfplwf" /v Start
    & reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet001\Control\Session Manager\Memory Management" /v PagingFiles
    & reg.exe query "HKLM\OFFLINE_SYSTEM\ControlSet001\Services\Tcpip\Parameters" /v DisableDHCPMediaSense
    Write-Host "MountedDevices after wipe:"
    & reg.exe query "HKLM\OFFLINE_SYSTEM\MountedDevices"

}
catch {
    Write-Error "ERROR during registry patching: $_"
}
finally {
    # 5. Cleanly unload hive
    Write-Host "[6/6] Unloading hive and dismounting VHD..."
    [gc]::Collect()
    [gc]::WaitForPendingFinalizers()
    Start-Sleep -Milliseconds 500

    $unloadAttempts = 0
    $unloaded = $false
    while (-not $unloaded -and $unloadAttempts -lt 5) {
        $unloadAttempts++
        & reg.exe unload HKLM\OFFLINE_SYSTEM
        if ($LASTEXITCODE -eq 0) {
            $unloaded = $true
            Write-Host "Hive HKLM\OFFLINE_SYSTEM unloaded successfully."
        } else {
            Write-Warning "Unload attempt $unloadAttempts failed ($LASTEXITCODE). Retrying in 1s..."
            Start-Sleep -Seconds 1
        }
    }

    # Dismount VHD
    Start-Sleep -Seconds 1
    Dismount-DiskImage -ImagePath $VhdPath -ErrorAction SilentlyContinue
    Write-Host "VHD dismounted cleanly."
    Write-Host "=== Fix Completed Successfully ===" -ForegroundColor Green
    Stop-Transcript
}
