<#
.SYNOPSIS
    Builds the in-RAM WinPE staging image used for zero-touch hardware provisioning.

.DESCRIPTION
    Produces the static asset set served by GET /static/wim/:

        wimboot      iPXE RAM-disk loader (ipxe.org)
        bootmgr      Legacy BIOS boot manager (from the ADK WinPE media)
        bcd          Legacy BIOS BCD, retargeted to \Windows\system32\boot\winload.exe
        boot.sdi     RAM disk template
        boot.wim     Compressed WinPE image with startnet.cmd + generic NIC drivers

    The resulting boot.wim is what a client boots when /api/boot-script decides it
    is not READY yet. It never touches the target VHD: drivers are injected into
    the VHD's offline SYSTEM hive only after POST /api/client/staged succeeds.

.PARAMETER DriverRoot
    Directory scanned recursively for driver INF files. It must contain generic
    (hardware-independent) NIC drivers at minimum for:
        Realtek  RTL8168 / RTL8125  (PCI\VEN_10EC&DEV_8168 / 8125)
        Intel    e1d / e1r / i225   (PCI\VEN_8086&DEV_15B8 and friends)

.PARAMETER WimbootPath
    Path to the iPXE wimboot binary. Download it from
    https://github.com/ipxe/ipxe/releases and pass it here (or drop it next to
    this script named "wimboot").

.EXAMPLE
    .\build_winpe.ps1 -DriverRoot D:\driver_pool -AssetOutput ..\..\assets\wim
#>
[CmdletBinding()]
param(
    # Root of the Windows ADK "Windows Preinstallation Environment" component.
    [string]$AdkWinPeRoot,

    # Target architecture of the WinPE image.
    [ValidateSet('amd64', 'x86')]
    [string]$Arch = 'amd64',

    # WinPE media language folder.
    [string]$Language = 'en-us',

    # Recursively-scanned driver pool (INF files) injected into the image.
    [string]$DriverRoot,

    # startnet.cmd copied into \Windows\System32 of the image.
    [string]$StartNetSource,

    # iPXE wimboot binary used as the iPXE `kernel`.
    [string]$WimbootPath,

    # Scratch directory for mount points and intermediate WIMs.
    [string]$WorkRoot,

    # Final boot.wim (also copied into the asset output directory).
    [string]$BootWimOutput,

    # Directory that receives wimboot / bootmgr / bcd / boot.sdi / boot.wim.
    [string]$AssetOutput,

    # Keep the WinPE image mounted after the run (debugging only).
    [switch]$KeepMounted
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# $PSScriptRoot is not populated while the param() defaults are evaluated, so
# every path-derived default is resolved here instead.
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $DriverRoot) { $DriverRoot = Join-Path $ScriptRoot 'driver_pool' }
if (-not $StartNetSource) { $StartNetSource = Join-Path $ScriptRoot 'startnet.cmd' }
if (-not $WimbootPath) { $WimbootPath = Join-Path $ScriptRoot 'wimboot' }
if (-not $WorkRoot) { $WorkRoot = Join-Path $ScriptRoot 'work' }
if (-not $AssetOutput) { $AssetOutput = Join-Path (Split-Path -Parent $ScriptRoot) 'assets\wim' }

function Write-Step {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host ('==> ' + $Message) -ForegroundColor Cyan
}

function Write-Info {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host ('    ' + $Message)
}

function Invoke-Tool {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string[]]$ArgumentList,
        [Parameter(Mandatory)][string]$Label
    )
    Write-Info $Label
    $output = & $FilePath @ArgumentList 2>&1
    $code = $LASTEXITCODE
    foreach ($line in $output) {
        Write-Host ('      ' + $line)
    }
    if ($code -ne 0) {
        throw ('{0} failed with exit code {1}' -f $Label, $code)
    }
}

function Find-AdkMediaRoot {
    param([string]$Root, [string]$TargetArch, [string]$TargetLang)

    $candidates = @(
        (Join-Path (Join-Path (Join-Path $Root $TargetArch) $TargetLang) 'Media'),
        (Join-Path (Join-Path $Root $TargetArch) 'Media')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'Sources\boot.wim')) {
            return $candidate
        }
    }
    $message = ('Cannot locate WinPE media under "{0}". Install the Windows ADK plus the ' +
        '"Windows Preinstallation Environment" add-on, or pass -AdkWinPeRoot.') -f $Root
    throw $message
}

# --- 0. Preconditions ---------------------------------------------------------

Write-Step 'Resolving inputs'

$Dism = Join-Path $env:SystemRoot 'System32\dism.exe'
if (-not (Test-Path $Dism)) { throw ('dism.exe not found at {0}' -f $Dism) }

if (-not $AdkWinPeRoot) {
    $adkRoots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment'),
        (Join-Path $env:ProgramFiles 'Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment')
    ) | Where-Object { $_ -and (Test-Path $_) }
    if (-not $adkRoots) {
        throw ('Windows ADK WinPE not found. Pass -AdkWinPeRoot explicitly.')
    }
    $AdkWinPeRoot = $adkRoots[0]
}

$MediaRoot = Find-AdkMediaRoot -Root $AdkWinPeRoot -TargetArch $Arch -TargetLang $Language
$WinPeSourceWim = Join-Path $MediaRoot 'Sources\boot.wim'
$SourceBootMgr = Join-Path $MediaRoot 'Boot\bootmgr'
$SourceBcd = Join-Path $MediaRoot 'Boot\BCD'
$SourceBootSdi = Join-Path $MediaRoot 'Boot\boot.sdi'

foreach ($required in @($WinPeSourceWim, $SourceBcd, $SourceBootSdi)) {
    if (-not (Test-Path $required)) { throw ('Required ADK file missing: {0}' -f $required) }
}
if (-not (Test-Path $SourceBootMgr)) {
    # Some ADK builds expose bootmgr.exe inside the image instead of the media root.
    $SourceBootMgr = $null
}
if (-not (Test-Path $StartNetSource)) {
    throw ('startnet.cmd not found at {0}. It is required for the staging flow.' -f $StartNetSource)
}
if (-not (Test-Path $WimbootPath)) {
    $message = ('wimboot not found at {0}. Download it from ' +
        'https://github.com/ipxe/ipxe/releases and pass -WimbootPath.') -f $WimbootPath
    throw $message
}
if (-not (Test-Path $DriverRoot)) {
    throw ('Driver pool not found at {0}' -f $DriverRoot)
}

if (-not $BootWimOutput) {
    $BootWimOutput = Join-Path $WorkRoot 'boot.wim'
}

$MountDir = Join-Path $WorkRoot ('mount_' + $Arch)
$StagingDir = Join-Path $WorkRoot 'staging'

Write-Info ('ADK media      : {0}' -f $MediaRoot)
Write-Info ('Driver pool    : {0}' -f $DriverRoot)
Write-Info ('Boot WIM output: {0}' -f $BootWimOutput)
Write-Info ('Asset output   : {0}' -f $AssetOutput)

# --- 1. Verify the generic NIC driver pool -----------------------------------

Write-Step 'Verifying generic NIC driver coverage'

$Infs = @(Get-ChildItem -Path $DriverRoot -Recurse -Filter '*.inf' -File -ErrorAction SilentlyContinue)
if ($Infs.Count -eq 0) {
    throw ('No .inf files found under {0}. Generic Realtek/Intel NIC drivers are mandatory.' -f $DriverRoot)
}

$RequiredPatterns = [ordered]@{
    'Realtek RTL8168 (PCI\VEN_10EC&DEV_8168)' = 'VEN_10EC&DEV_8168'
    'Realtek RTL8125 (PCI\VEN_10EC&DEV_8125)' = 'VEN_10EC&DEV_8125'
    'Intel e1d/i225  (PCI\VEN_8086)'          = 'VEN_8086'
}

$DriverText = ($Infs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
$Missing = @()
foreach ($entry in $RequiredPatterns.GetEnumerator()) {
    if ($DriverText -match [regex]::Escape($entry.Value)) {
        Write-Info ('FOUND  {0}' -f $entry.Key)
    } else {
        $Missing += $entry.Key
        $message = ('MISSING {0} - Windows will fall back to the CriticalDeviceDatabase ' +
            'binding injected by cob-armer, but an in-box generic driver is strongly ' +
            'preferred.') -f $entry.Key
        Write-Warning $message
    }
}
Write-Info ('Driver INF files discovered: {0}' -f $Infs.Count)

# --- 2. Mount the clean WinPE image ------------------------------------------

Write-Step 'Mounting clean WinPE image'

New-Item -ItemType Directory -Force -Path $MountDir, $StagingDir, $WorkRoot | Out-Null

Invoke-Tool -FilePath $Dism -Label 'dism /Mount-Image' -ArgumentList @(
    '/Mount-Image',
    ('/ImageFile:{0}' -f $WinPeSourceWim),
    '/Index:1',
    ('/MountDir:{0}' -f $MountDir)
)

$Mounted = $true
try {
    # --- 3. Drop startnet.cmd into the image --------------------------------
    Write-Step 'Installing startnet.cmd'
    $TargetStartNet = Join-Path $MountDir 'Windows\System32\startnet.cmd'
    Copy-Item -LiteralPath $StartNetSource -Destination $TargetStartNet -Force
    Write-Info $TargetStartNet

    # --- 4. Inject generic NIC drivers --------------------------------------
    Write-Step 'Injecting generic NIC drivers into WinPE'
    Invoke-Tool -FilePath $Dism -Label 'dism /Add-Driver /Recurse' -ArgumentList @(
        ('/Image:{0}' -f $MountDir),
        ('/Driver:{0}' -f $DriverRoot),
        '/Recurse',
        '/Quiet'
    )

    # --- 5. Commit ----------------------------------------------------------
    if ($KeepMounted) {
        Write-Step 'Leaving image mounted (-KeepMounted)'
        $Mounted = $false
    } else {
        Write-Step 'Committing WinPE image'
        Invoke-Tool -FilePath $Dism -Label 'dism /Unmount-Image /Commit' -ArgumentList @(
            '/Unmount-Image',
            ('/MountDir:{0}' -f $MountDir),
            '/Commit'
        )
        $Mounted = $false
    }
} finally {
    if ($Mounted) {
        Write-Warning 'Rolling back the mounted WinPE image'
        & $Dism /Unmount-Image /MountDir:$MountDir /Discard | Out-Null
    }
}

if ($KeepMounted) {
    Write-Host ''
    Write-Host 'Image left mounted for inspection. Re-run without -KeepMounted to finish.' -ForegroundColor Yellow
    return
}

# --- 6. Export a minimal compressed boot.wim ---------------------------------

Write-Step 'Exporting compressed boot.wim'

$TempWim = Join-Path $StagingDir 'boot.export.wim'
if (Test-Path $TempWim) { Remove-Item -LiteralPath $TempWim -Force }

Invoke-Tool -FilePath $Dism -Label 'dism /Export-Image /Compress:max' -ArgumentList @(
    '/Export-Image',
    ('/ImageFile:{0}' -f $WinPeSourceWim),
    '/Index:1',
    ('/DestinationFile:{0}' -f $TempWim),
    '/Compress:max',
    '/CheckIntegrity'
)

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $BootWimOutput) | Out-Null
Move-Item -LiteralPath $TempWim -Destination $BootWimOutput -Force
Write-Info ('boot.wim -> {0} ({1:N1} MB)' -f $BootWimOutput,
    ((Get-Item -LiteralPath $BootWimOutput).Length / 1MB))

# --- 7. Stage the HTTP asset set --------------------------------------------

Write-Step 'Staging static /static/wim assets'

New-Item -ItemType Directory -Force -Path $AssetOutput | Out-Null

Copy-Item -LiteralPath $WimbootPath -Destination (Join-Path $AssetOutput 'wimboot') -Force
Copy-Item -LiteralPath $SourceBcd -Destination (Join-Path $AssetOutput 'bcd') -Force
Copy-Item -LiteralPath $SourceBootSdi -Destination (Join-Path $AssetOutput 'boot.sdi') -Force
Copy-Item -LiteralPath $BootWimOutput -Destination (Join-Path $AssetOutput 'boot.wim') -Force

if ($SourceBootMgr) {
    Copy-Item -LiteralPath $SourceBootMgr -Destination (Join-Path $AssetOutput 'bootmgr') -Force
} else {
    $message = 'bootmgr was not found in the ADK media. Copy it manually into {0}.' -f (Join-Path $AssetOutput 'bootmgr')
    Write-Warning $message
}

# --- 8. Legacy BIOS BCD retargeting ------------------------------------------

Write-Step 'Rewriting BCD for Legacy BIOS (MBR) boot'

$BcdEdit = Join-Path $env:SystemRoot 'System32\bcdedit.exe'
$StagedBcd = Join-Path $AssetOutput 'bcd'

# wimboot exposes each initrd under the alias we hand it in the iPXE script, so
# the ramdisk path must match the alias "boot.wim" exactly.
$BcdEdits = @(
    @('/store', $StagedBcd, '/set', '{ramdiskoptions}', 'ramdisksdidevice', 'boot'),
    @('/store', $StagedBcd, '/set', '{ramdiskoptions}', 'ramdisksdipath', '\boot.wim'),
    @('/store', $StagedBcd, '/set', '{default}', 'device', 'ramdisk=[boot]\boot.wim,{ramdiskoptions}'),
    @('/store', $StagedBcd, '/set', '{default}', 'osdevice', 'ramdisk=[boot]\boot.wim,{ramdiskoptions}'),
    # Legacy BIOS entry point - the whole point of this BCD.
    @('/store', $StagedBcd, '/set', '{default}', 'path', '\Windows\system32\boot\winload.exe'),
    @('/store', $StagedBcd, '/set', '{default}', 'systemroot', '\Windows'),
    @('/store', $StagedBcd, '/set', '{default}', 'winpe', 'true'),
    @('/store', $StagedBcd, '/set', '{default}', 'detecthal', 'true'),
    @('/store', $StagedBcd, '/set', '{bootmgr}', 'device', 'boot'),
    @('/store', $StagedBcd, '/set', '{bootmgr}', 'timeout', '3')
)

foreach ($arguments in $BcdEdits) {
    $label = 'bcdedit ' + ($arguments -join ' ')
    Invoke-Tool -FilePath $BcdEdit -ArgumentList $arguments -Label $label
}

# --- 9. Summary ---------------------------------------------------------------

Write-Host ''
Write-Host 'WinPE staging image built successfully.' -ForegroundColor Green
Get-ChildItem -Path $AssetOutput -File | ForEach-Object {
    Write-Host ('  {0,-12} {1,12:N0} bytes' -f $_.Name, $_.Length)
}

if ($Missing.Count -gt 0) {
    Write-Host ''
    Write-Warning ('{0} driver family missing from the pool. See warnings above.' -f $Missing.Count)
}
