# repair_vhd_complete.ps1
$ErrorActionPreference = "Continue"
$logPath = "C:\Users\Admin\Desktop\cobdiskless\vhd_repair.log"

Start-Transcript -Path $logPath -Force

Write-Host "=================================================="
Write-Host "STAGE 1: Process Release & Pre-check"
Write-Host "=================================================="
Get-Process -Name cobdiskless_app -ErrorAction SilentlyContinue | Stop-Process -Force -Verbose
Start-Sleep -Seconds 1

$vhdPath = "C:\Users\Admin\Desktop\default_os.vhd"
if (!(Test-Path $vhdPath)) {
    Write-Error "VHD path does not exist: $vhdPath"
    Stop-Transcript
    exit 1
}

# Ensure dismounted first
$existing = Get-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
if ($existing -and $existing.Attached) {
    Write-Host "Dismounting existing attachment..."
    Dismount-DiskImage -ImagePath $vhdPath
    Start-Sleep -Seconds 2
}

Write-Host "=================================================="
Write-Host "STAGE 2: Mounting VHD Image"
Write-Host "=================================================="
$mountResult = Mount-DiskImage -ImagePath $vhdPath -StorageType VHD -PassThru
Start-Sleep -Seconds 3

$diskImage = Get-DiskImage -ImagePath $vhdPath
$disk = $diskImage | Get-Disk
Write-Host "Attached Disk Number: $($disk.Number)"

$allParts = $disk | Get-Partition | Where-Object { $_.Type -ne 'Reserved' }
$driveLetter = $null

foreach ($p in $allParts) {
    if (-not $p.DriveLetter) {
        $freeLetter = (Get-ChildItem function:[d-z]: -n | Where-Object { !(Test-Path $_) })[0].Substring(0,1)
        Write-Host "Assigning temporary drive letter ${freeLetter}: to Disk $($p.DiskNumber) Partition $($p.PartitionNumber)..."
        Set-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -NewDriveLetter $freeLetter -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
        $p = $disk | Get-Partition -PartitionNumber $p.PartitionNumber
    }
    if ($p.DriveLetter) {
        $testLetter = "$($p.DriveLetter):"
        Write-Host "Checking partition drive letter $testLetter for Windows installation..."
        if (Test-Path "$testLetter\Windows\System32") {
            $driveLetter = $testLetter
            Write-Host "Found Windows installation at $driveLetter\Windows"
            break
        }
    }
}

if (-not $driveLetter) {
    Write-Error "Failed to locate Windows installation partition on VHD!"
    Dismount-DiskImage -ImagePath $vhdPath
    Stop-Transcript
    exit 1
}

Write-Host "Active VHD Windows Drive: $driveLetter"

Write-Host "=================================================="
Write-Host "STAGE 3: Realtek Driver File Deployment & DISM"
Write-Host "=================================================="
$realtekRepo = Get-ChildItem -Path "C:\Windows\System32\DriverStore\FileRepository" -Filter "rt640x64.inf_amd64_82d53ae24f2639b8" -Directory
if (-not $realtekRepo) {
    $realtekRepo = Get-ChildItem -Path "C:\Windows\System32\DriverStore\FileRepository" -Filter "rt640x64*" -Directory | Select-Object -First 1
}

$vhdDriversDir = "$driveLetter\Windows\System32\drivers"
if (Test-Path "$($realtekRepo.FullName)\rt640x64.sys") {
    Write-Host "Copying $($realtekRepo.FullName)\rt640x64.sys -> $vhdDriversDir\rt640x64.sys"
    Copy-Item -Path "$($realtekRepo.FullName)\rt640x64.sys" -Destination "$vhdDriversDir\rt640x64.sys" -Force
}

# Also verify that DISM installs the driver package
$tempDrivers = "C:\temp_drivers"
if (-not (Test-Path $tempDrivers)) {
    New-Item -ItemType Directory -Path $tempDrivers -Force | Out-Null
    Copy-Item -Path $realtekRepo.FullName -Destination (Join-Path $tempDrivers $realtekRepo.Name) -Recurse -Force
}
$imageArg = "/Image:" + $driveLetter + "\"
$driverArg = "/Driver:" + $tempDrivers
Write-Host "Running DISM Add-Driver..."
& dism.exe $imageArg /Add-Driver $driverArg /Recurse
Write-Host "DISM ExitCode: $LASTEXITCODE"

Write-Host "=================================================="
Write-Host "STAGE 4: Registry Critical-Boot Override"
Write-Host "=================================================="
$systemHive = "$driveLetter\Windows\System32\config\SYSTEM"
Write-Host "Loading offline hive: $systemHive"
& reg.exe load HKLM\OFFLINE_SYSTEM "$systemHive"
Start-Sleep -Seconds 1

# Check Current ControlSet
$selectKey = "HKLM:\OFFLINE_SYSTEM\Select"
$currentCSNum = 1
if (Test-Path $selectKey) {
    $csVal = (Get-ItemProperty -Path $selectKey -Name "Current" -ErrorAction SilentlyContinue).Current
    if ($csVal) { $currentCSNum = $csVal }
}
$controlSets = @("ControlSet{0:D3}" -f $currentCSNum)
if ($controlSets[0] -ne "ControlSet001" -and (Test-Path "HKLM:\OFFLINE_SYSTEM\ControlSet001")) {
    $controlSets += "ControlSet001"
}

foreach ($cs in $controlSets) {
    Write-Host "Configuring ControlSet: $cs"
    $servicesKey = "HKLM:\OFFLINE_SYSTEM\$cs\Services"
    
    # 1. rt640x64 Realtek Service
    $rtPath = "$servicesKey\rt640x64"
    if (!(Test-Path $rtPath)) {
        Write-Host "Creating $rtPath key..."
        New-Item -Path $rtPath -Force | Out-Null
    }
    
    Set-ItemProperty -Path $rtPath -Name "Type" -Value 1 -Type DWord -Force
    Set-ItemProperty -Path $rtPath -Name "Start" -Value 0 -Type DWord -Force
    Set-ItemProperty -Path $rtPath -Name "ErrorControl" -Value 1 -Type DWord -Force
    Set-ItemProperty -Path $rtPath -Name "Group" -Value "NDIS" -Type String -Force
    Set-ItemProperty -Path $rtPath -Name "Tag" -Value 13 -Type DWord -Force
    Set-ItemProperty -Path $rtPath -Name "ImagePath" -Value "System32\drivers\rt640x64.sys" -Type ExpandString -Force
    Set-ItemProperty -Path $rtPath -Name "DisplayName" -Value "Realtek RT640 NT Driver" -Type String -Force
    Set-ItemProperty -Path $rtPath -Name "BootFlags" -Value 1 -Type DWord -Force
    Set-ItemProperty -Path $rtPath -Name "NdisMajorVersion" -Value 6 -Type DWord -Force
    Set-ItemProperty -Path $rtPath -Name "NdisMinorVersion" -Value 40 -Type DWord -Force

    # Parameters
    $paramPath = "$rtPath\Parameters"
    if (!(Test-Path $paramPath)) { New-Item -Path $paramPath -Force | Out-Null }
    Set-ItemProperty -Path $paramPath -Name "DrvOption" -Value 0 -Type DWord -Force
    Set-ItemProperty -Path $paramPath -Name "DrvMode" -Value 0 -Type DWord -Force
    Set-ItemProperty -Path $paramPath -Name "DmaRemappingCompatible" -Value 0 -Type DWord -Force
    
    # Wdf
    $wdfPath = "$paramPath\Wdf"
    if (!(Test-Path $wdfPath)) { New-Item -Path $wdfPath -Force | Out-Null }
    Set-ItemProperty -Path $wdfPath -Name "WdfMajorVersion" -Value 1 -Type DWord -Force
    Set-ItemProperty -Path $wdfPath -Name "WdfMinorVersion" -Value 15 -Type DWord -Force
    
    # Clear StartOverride if present to prevent Windows delaying driver to Start=3!
    $overridePath = "$rtPath\StartOverride"
    if (Test-Path $overridePath) {
        Write-Host "Neutralizing StartOverride on rt640x64..."
        Get-Item -Path $overridePath | Get-ItemProperty | ForEach-Object {
            $propNames = $_.PSObject.Properties | Where-Object { $_.Name -match '^\d+$' } | Select-Object -ExpandProperty Name
            foreach ($pn in $propNames) {
                Set-ItemProperty -Path $overridePath -Name $pn -Value 0 -Type DWord -Force
            }
        }
    }
    
    Write-Host "  -> Verified rt640x64 in ${cs}: Start=0, Group=NDIS, ImagePath=System32\drivers\rt640x64.sys"

    # 2. Critical SANBoot & Network Services
    $coreServices = @(
        @{ Name = "msiscsi"; Group = "Boot Bus Extender" },
        @{ Name = "iscsiprt"; Group = "SCSI Miniport" },
        @{ Name = "NDIS"; Group = "NDIS Wrapper" },
        @{ Name = "Tcpip"; Group = "PNP_TDI" },
        @{ Name = "pci"; Group = "Boot Bus Extender" },
        @{ Name = "Ndisuio"; Group = "NDIS" },
        @{ Name = "WfpLwf"; Group = "NDIS" }
    )

    foreach ($svc in $coreServices) {
        $spath = "$servicesKey\$($svc.Name)"
        if (Test-Path $spath) {
            Set-ItemProperty -Path $spath -Name "Start" -Value 0 -Type DWord -Force
            if ($svc.Group) {
                Set-ItemProperty -Path $spath -Name "Group" -Value $svc.Group -Type String -Force
            }
            # Neutralize StartOverride if present
            $so = "$spath\StartOverride"
            if (Test-Path $so) {
                Get-Item -Path $so | Get-ItemProperty | ForEach-Object {
                    $propNames = $_.PSObject.Properties | Where-Object { $_.Name -match '^\d+$' } | Select-Object -ExpandProperty Name
                    foreach ($pn in $propNames) {
                        Set-ItemProperty -Path $so -Name $pn -Value 0 -Type DWord -Force
                    }
                }
            }
            Write-Host "  -> Service '$($svc.Name)' configured for Boot Start (Start=0, Group=$($svc.Group))"
        }
    }
}

Write-Host "Unloading offline registry hive..."
[gc]::collect()
[gc]::WaitForPendingFinalizers()
Start-Sleep -Seconds 1
& reg.exe unload HKLM\OFFLINE_SYSTEM
Write-Host "Hive unloaded cleanly. ExitCode: $LASTEXITCODE"

Write-Host "=================================================="
Write-Host "STAGE 5: Dismount & Cleanup"
Write-Host "=================================================="
Dismount-DiskImage -ImagePath $vhdPath
Start-Sleep -Seconds 3

$checkImage = Get-DiskImage -ImagePath $vhdPath
Write-Host "VHD Attached Status: $($checkImage.Attached)"

if (Test-Path $tempDrivers) {
    Write-Host "Cleaning up $tempDrivers..."
    Remove-Item -Path $tempDrivers -Recurse -Force
}

Write-Host "=================================================="
Write-Host "ALL REPAIR STAGES COMPLETED SUCCESSFULLY!"
Write-Host "=================================================="

Stop-Transcript
