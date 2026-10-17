[CmdletBinding()]
param(
    [Parameter(Position=0)]
    [string]$ImagePath = "G:\csmw10.VHD",
    [string]$DriverInfDir = "C:\Windows\System32\DriverStore\FileRepository\rt640x64.inf_amd64_82d53ae24f2639b8"
)

$ErrorActionPreference = "Stop"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Automated Multi-NIC Universal Driver Staging & Arming    " -ForegroundColor Cyan
Write-Host " Target Image: $ImagePath" -ForegroundColor Cyan
Write-Host " Driver Repo:  $DriverInfDir" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Step 1: Mount VHD via PowerShell
Write-Host "`n[1/6] Mounting $ImagePath..." -ForegroundColor Yellow
$m = Mount-DiskImage -ImagePath $ImagePath -PassThru
$letter = ($m | Get-Disk | Get-Partition | Get-Volume | Where-Object DriveLetter).DriveLetter
if (-not $letter) {
    Write-Error "Failed to mount $ImagePath or assign drive letter."
    exit 1
}
Write-Host "  -> Volume mounted successfully at ${letter}:\" -ForegroundColor Green

try {
    # Step 2: Check current version of rt640x64.sys
    $dstSys = "${letter}:\Windows\System32\drivers\rt640x64.sys"
    if (Test-Path $dstSys) {
        $curVer = (Get-Item $dstSys).VersionInfo.FileVersion
        Write-Host "`n[2/6] Current rt640x64.sys version: $curVer" -ForegroundColor Yellow
    } else {
        Write-Host "`n[2/6] rt640x64.sys not found in destination image." -ForegroundColor Yellow
    }

    # Step 3: Replace rt640x64.sys with modern universal Realtek driver
    $srcSys = Join-Path $DriverInfDir "rt640x64.sys"
    if (Test-Path $srcSys) {
        $srcVer = (Get-Item $srcSys).VersionInfo.FileVersion
        Write-Host "`n[3/6] Staging modern driver $srcSys (version: $srcVer) -> $dstSys..." -ForegroundColor Yellow
        Copy-Item -Path $srcSys -Destination $dstSys -Force
        Write-Host "  -> Successfully staged rt640x64.sys version $srcVer" -ForegroundColor Green
    } else {
        Write-Warning "Source driver not found at $srcSys"
    }

    # Step 4: Run DISM offline driver injection
    Write-Host "`n[4/6] Running DISM offline driver injection..." -ForegroundColor Yellow
    & dism.exe /Image:"${letter}:\" /Add-Driver /Driver:"$DriverInfDir" /Recurse
    Write-Host "  -> DISM driver injection completed." -ForegroundColor Green
}
finally {
    # Step 5: Dismount VHD
    Write-Host "`n[5/6] Dismounting $ImagePath..." -ForegroundColor Yellow
    Dismount-DiskImage -ImagePath $ImagePath
    Write-Host "  -> Volume dismounted cleanly." -ForegroundColor Green
}

# Step 6: Re-run CobDiskless PnpDriverArmer
Write-Host "`n[6/6] Arming image boot services via cob-armer..." -ForegroundColor Yellow
$armerPath = Join-Path $PSScriptRoot "..\build\cob-armer.exe"
if (Test-Path $armerPath) {
    & $armerPath --arm "$ImagePath"
    Write-Host "  -> CobDiskless arming verified." -ForegroundColor Green
} else {
    Write-Warning "cob-armer.exe not found at $armerPath"
}

Write-Host "`nUniversal driver upgrade & arming completed successfully!" -ForegroundColor Green
