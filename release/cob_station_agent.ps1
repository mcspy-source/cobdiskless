<#
.SYNOPSIS
    CobDiskless Station Agent - Hostname Provisioning & Auto-Rename Helper
.DESCRIPTION
    Runs on diskless client boot. Detects active physical MAC address,
    queries the CobDiskless daemon station API (GET /api/v1/station?mac=...),
    compares the assigned name with $env:COMPUTERNAME, and renames & restarts
    the computer if they differ.
#>
[CmdletBinding()]
param(
    [string]$Server = $env:COBDISKLESS_SERVER,
    [int]$Port = $(if ($env:COBDISKLESS_HTTP_PORT) { [int]$env:COBDISKLESS_HTTP_PORT } else { 18080 })
)

$ErrorActionPreference = 'Stop'

# Ensure administrative privileges
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "CobDiskless Station Agent requires Administrator privileges. Relaunching elevated..."
    Start-Process powershell.exe -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"{0}`"" -f $MyInvocation.MyCommand.Path) -Verb RunAs
    exit 0
}

# Resolve CobDiskless server IP if not provided
if (-not $Server) {
    # Attempt to resolve default gateway or DHCP server
    $gw = (Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty NextHop -First 1)
    if ($gw) {
        $Server = $gw
    } else {
        $Server = '192.168.1.100'
    }
}

# Detect active physical MAC address
$mac = $null
try {
    $adapter = Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
        Where-Object { $_.Status -eq 'Up' } |
        Select-Object -First 1
    if (-not $adapter) {
        $adapter = Get-NetAdapter -ErrorAction SilentlyContinue |
            Where-Object { $_.Status -eq 'Up' -and $_.HardwareInterface -eq $true } |
            Select-Object -First 1
    }
    if ($adapter) {
        $mac = $adapter.MacAddress.Replace('-', ':').ToUpper()
    }
} catch {
    # Fallback to getmac
    $rawMac = (getmac /fo csv /nh 2>$null | Select-Object -First 1)
    if ($rawMac -match '([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}') {
        $mac = $matches[0].Replace('-', ':').ToUpper()
    }
}

if (-not $mac) {
    Write-Error "[cob_station_agent] Unable to detect active physical MAC address."
    exit 1
}

Write-Host "[cob_station_agent] Detected MAC address: $mac"
Write-Host "[cob_station_agent] Connecting to http://${Server}:${Port}/api/v1/station?mac=$mac..."

$apiUrl = "http://${Server}:${Port}/api/v1/station?mac=$mac"
$response = $null
try {
    $response = Invoke-RestMethod -Uri $apiUrl -Method Get -TimeoutSec 10
} catch {
    Write-Error "[cob_station_agent] Failed to query station API: $_"
    exit 1
}

if (-not $response -or -not $response.name) {
    Write-Error "[cob_station_agent] Server returned empty station name."
    exit 1
}

$assignedName = $response.name.Trim()
$currentName = $env:COMPUTERNAME

Write-Host "[cob_station_agent] Assigned Name: $assignedName | Current Name: $currentName"

if ($assignedName -ieq $currentName) {
    Write-Host "[cob_station_agent] Hostname is already up to date ($currentName). No action needed."
    exit 0
}

Write-Host "[cob_station_agent] Renaming computer from $currentName to $assignedName..."
try {
    # Update registry values for instant consistency
    $regPaths = @(
        "HKLM:\SYSTEM\CurrentControlSet\Control\ComputerName\ComputerName",
        "HKLM:\SYSTEM\CurrentControlSet\Control\ComputerName\ActiveComputerName"
    )
    foreach ($path in $regPaths) {
        if (Test-Path $path) {
            Set-ItemProperty -Path $path -Name "ComputerName" -Value $assignedName -Force
        }
    }
    $tcpipParams = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters"
    if (Test-Path $tcpipParams) {
        Set-ItemProperty -Path $tcpipParams -Name "Hostname" -Value $assignedName -Force
        Set-ItemProperty -Path $tcpipParams -Name "NV Hostname" -Value $assignedName -Force
    }

    Rename-Computer -NewName $assignedName -Force
    Write-Host "[cob_station_agent] Computer successfully renamed to $assignedName. Restarting in 5 seconds..."
    Restart-Computer -Force
} catch {
    Write-Error "[cob_station_agent] Error renaming or restarting computer: $_"
    exit 1
}
