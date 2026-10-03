$ErrorActionPreference = "Continue"
$logPath = "C:\Users\Admin\Desktop\cobdiskless\patch_output.log"
Start-Transcript -Path $logPath -Force

Write-Host "=================================================="
Write-Host "PATCH OFFLINE VHD DRIVERS & SYSTEM REGISTRY"
Write-Host "=================================================="

# 0. Pre-flight: Stop cobdiskless_app so VHD is not locked
Get-Process -Name cobdiskless_app -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

# Ensure any previous hive or mount is cleaned up
& reg unload HKLM\VHD_SYSTEM 2>&1 | Out-Null

$vhdPath = "c:\Users\Admin\Desktop\default.vhd"
if (!(Test-Path $vhdPath)) {
    Write-Error "VHD path does not exist: $vhdPath"
    exit 1
}

# Ensure clean mount state
$existing = Get-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
if ($existing -and $existing.Attached) {
    Write-Host "Dismounting existing attachment..."
    Dismount-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

# 1. Mount the VHD
Write-Host "Mounting VHD: $vhdPath..."
$mount = Mount-DiskImage -ImagePath $vhdPath -PassThru
Start-Sleep -Seconds 2

$disk = Get-DiskImage -ImagePath $vhdPath | Get-Disk
Write-Host "Mounted Disk Number: $($disk.Number)"

# Ensure partitions have drive letters
$partitions = $disk | Get-Partition | Where-Object { $_.Type -ne 'Reserved' }
$vhdDrive = $null

foreach ($p in $partitions) {
    if (-not $p.DriveLetter) {
        $usedLetters = (Get-Volume | Where-Object DriveLetter | Select-Object -ExpandProperty DriveLetter)
        $avail = [char[]]([char]'D'..[char]'Z') | Where-Object { $_ -notin $usedLetters }
        if ($avail) {
            $newLetter = $avail[0]
            Write-Host "Assigning temporary drive letter ${newLetter}: to partition $($p.PartitionNumber)..."
            Set-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -NewDriveLetter $newLetter -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 1
        }
    }
}

# Re-query volumes to find the Windows installation
$vols = Get-DiskImage -ImagePath $vhdPath | Get-Disk | Get-Partition | Get-Volume | Where-Object DriveLetter
foreach ($v in $vols) {
    $testPath = "$($v.DriveLetter):\Windows\System32\config\SYSTEM"
    Write-Host "Checking volume $($v.DriveLetter): for SYSTEM hive..."
    if (Test-Path $testPath) {
        $vhdDrive = "$($v.DriveLetter):"
        Write-Host "Found Windows SYSTEM hive at $testPath" -ForegroundColor Green
        break
    }
}

if (-not $vhdDrive) {
    Write-Error "Could not locate Windows partition with SYSTEM hive on $vhdPath"
    Dismount-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
    exit 1
}

Write-Host "Mounted Windows partition at $vhdDrive" -ForegroundColor Cyan

# 2. Load Offline SYSTEM Hive
$systemHivePath = "$vhdDrive\Windows\System32\config\SYSTEM"
Write-Host "Loading offline hive from: $systemHivePath..."
$loadResult = & reg load HKLM\VHD_SYSTEM $systemHivePath 2>&1
Write-Host $loadResult

# 3. Detect CurrentControlSet
$selectKey = Get-ItemProperty -Path "HKLM:\VHD_SYSTEM\Select" -ErrorAction Stop
$currentControlSetNum = $selectKey.Current
$ccs = "ControlSet{0:D3}" -f $currentControlSetNum
Write-Host "Target ControlSet: $ccs (Current = $currentControlSetNum)" -ForegroundColor Cyan

# 4. Arm Core Boot-Start Storage & Network Drivers (Start = 0)
$criticalServices = @(
    "msiscsi",       # Microsoft iSCSI Initiator Driver
    "iscsiprt",      # iSCSI Port Driver
    "Tcpip",         # TCP/IP Protocol Driver
    "rt640x64",      # Realtek PCIe GBE Driver (Windows 10/11)
    "rt64win7",      # Realtek Legacy Driver (fallback)
    "NDIS"           # Network Driver Interface Specification
)

foreach ($svc in $criticalServices) {
    $path = "HKLM:\VHD_SYSTEM\$ccs\Services\$svc"
    if (Test-Path $path) {
        $oldStart = (Get-ItemProperty -Path $path -Name "Start" -ErrorAction SilentlyContinue).Start
        Set-ItemProperty -Path $path -Name "Start" -Value 0 -Type DWord
        $newStart = (Get-ItemProperty -Path $path -Name "Start" -ErrorAction SilentlyContinue).Start
        Write-Host "Set $svc -> Start: $oldStart -> $newStart (0 = BOOT)" -ForegroundColor Green
    } else {
        Write-Host "Service $svc not found in this hive, skipping." -ForegroundColor Yellow
    }
}

# 5. Disable Realtek Low Power / PHY Reset on Link Down
$realtekClassPath = "HKLM:\VHD_SYSTEM\$ccs\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}"
if (Test-Path $realtekClassPath) {
    Get-ChildItem -Path $realtekClassPath -ErrorAction SilentlyContinue | ForEach-Object {
        $driverDesc = (Get-ItemProperty -Path $_.PSPath -Name "DriverDesc" -ErrorAction SilentlyContinue).DriverDesc
        if ($driverDesc -like "*Realtek*") {
            Write-Host "Configuring Realtek parameters on $($_.PSChildName) ($driverDesc)..." -ForegroundColor Cyan
            Set-ItemProperty -Path $_.PSPath -Name "*EEE" -Value "0" -ErrorAction SilentlyContinue
            Set-ItemProperty -Path $_.PSPath -Name "GigaLite" -Value "0" -ErrorAction SilentlyContinue
            Set-ItemProperty -Path $_.PSPath -Name "PowerSavingMode" -Value "0" -ErrorAction SilentlyContinue
            Set-ItemProperty -Path $_.PSPath -Name "AutoDisablePhy" -Value "0" -ErrorAction SilentlyContinue
            Write-Host "  -> Disabled EEE, GigaLite, PowerSavingMode, AutoDisablePhy" -ForegroundColor Green
        }
    }
} else {
    Write-Host "Network adapter class path not found in hive." -ForegroundColor Yellow
}

# 6. Flush and Unload Hive
Write-Host "Flushing and unloading hive..."
[GC]::Collect()
[GC]::WaitForPendingFinalizers()
Start-Sleep -Seconds 1

$unloadResult = & reg unload HKLM\VHD_SYSTEM 2>&1
Write-Host $unloadResult

Write-Host "Dismounting VHD: $vhdPath..."
Dismount-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Host "=================================================="
Write-Host "VHD patched and dismounted successfully." -ForegroundColor Green
Write-Host "=================================================="
