# launch_dhcp_admin.ps1
# Starts cobdiskless_app.exe with the Administrator token required to bind UDP 67.
# Run this script from a normal PowerShell window; UAC will prompt once.

$exe = Join-Path $PSScriptRoot "build\cobdiskless_app.exe"

if (-not (Test-Path $exe)) {
    Write-Error "Binary not found: $exe"
    exit 1
}

# Check if already elevated
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
            ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($isAdmin) {
    Write-Host "[OK] Already elevated. Starting cobdiskless_app.exe ..."
    & $exe @args
} else {
    Write-Host "[INFO] Re-launching elevated (UAC prompt will appear) ..."
    $argString = if ($args.Count -gt 0) { $args -join ' ' } else { "" }
    Start-Process powershell -Verb RunAs -WorkingDirectory $PSScriptRoot -ArgumentList "-NoProfile -NoExit -Command `"& '$exe' $argString`""
}
