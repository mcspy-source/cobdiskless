# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "patch_network_stack.log"
Start-Transcript -Path $logPath -Force

$vhd = "G:\G\csmw10.VHD"
attrib -r $vhd
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadWrite | Out-Null
Start-Sleep -Seconds 2
$d = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $d.Number | Where-Object { $_.Type -ne 'Reserved' }
$vLetter = $null
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $testPath = "$($p.DriveLetter):\Windows\System32\config\SYSTEM"
        if (Test-Path $testPath) { $vLetter = "$($p.DriveLetter):"; break }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Y -ErrorAction SilentlyContinue
        if (Test-Path "Y:\Windows\System32\config\SYSTEM") { $vLetter = "Y:"; break }
    }
}

Write-Host "Mounted Windows root: $vLetter"
if (-not $vLetter) {
    Write-Error "Could not find Windows partition on VHD!"
    exit 1
}

& reg.exe load HKLM\ARM_SYS "$vLetter\Windows\System32\config\SYSTEM"

$adapters = @(
    @{
        Guid = "{A1B2C3D4-E5F6-4A1B-8C2D-000000000001}"
        Name = "Ethernet Intel e1000"
        PnPInstance = "PCI\VEN_8086&DEV_100E&SUBSYS_001E8086&REV_00\0018"
        ClassIndex = "0002"
    },
    @{
        Guid = "{A1B2C3D4-E5F6-4A1B-8C2D-000000000002}"
        Name = "Ethernet Realtek 8139"
        PnPInstance = "PCI\VEN_10EC&DEV_8139&SUBSYS_00000000&REV_20\0018"
        ClassIndex = "0003"
    },
    @{
        Guid = "{A1B2C3D4-E5F6-4A1B-8C2D-000000000003}"
        Name = "Ethernet Intel e1000e"
        PnPInstance = "PCI\VEN_8086&DEV_10D3&SUBSYS_00000000&REV_00\0018"
        ClassIndex = "0004"
    }
)

# 1. Update Control\Network\{4d36e972-e325-11ce-bfc1-08002be10318}
foreach ($ad in $adapters) {
    $netKey = "HKLM\ARM_SYS\ControlSet001\Control\Network\{4d36e972-e325-11ce-bfc1-08002be10318}\$($ad.Guid)\Connection"
    Write-Host "Creating Network Connection: $netKey"
    & reg.exe add $netKey /v "Name" /t REG_SZ /d $ad.Name /f
    & reg.exe add $netKey /v "PnPInstanceId" /t REG_SZ /d $ad.PnPInstance /f
    & reg.exe add $netKey /v "DefaultGatewayMac" /t REG_BINARY /d "000000000000" /f

    # Add Tcpip\Parameters\Interfaces entry
    $ifKey = "HKLM\ARM_SYS\ControlSet001\Services\Tcpip\Parameters\Interfaces\$($ad.Guid)"
    Write-Host "Creating Tcpip Interface: $ifKey"
    & reg.exe add $ifKey /v "EnableDHCP" /t REG_DWORD /d 1 /f
    & reg.exe add $ifKey /v "NameServer" /t REG_SZ /d "" /f
    & reg.exe add $ifKey /v "Domain" /t REG_SZ /d "" /f
    & reg.exe add $ifKey /v "AddressType" /t REG_DWORD /d 0 /f

    # Enhance Class Linkage
    $classLinkKey = "HKLM\ARM_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\$($ad.ClassIndex)\Linkage"
    Write-Host "Updating Class Linkage: $classLinkKey"
    & reg.exe add $classLinkKey /v "Export" /t REG_MULTI_SZ /d "\Device\$($ad.Guid)" /f
    & reg.exe add $classLinkKey /v "UpperBind" /t REG_MULTI_SZ /d "Ndisuio\0RasPppoe\0RDMANDK\0Tcpip\0Tcpip6" /f
}

# 2. Update Tcpip\Linkage to bind ALL adapters
$tcpExports = @(
    "\Device\Tcpip_{820C4568-D585-420E-94B3-AEA0E0DC3402}",
    "\Device\Tcpip_{A1B2C3D4-E5F6-4A1B-8C2D-000000000001}",
    "\Device\Tcpip_{A1B2C3D4-E5F6-4A1B-8C2D-000000000002}",
    "\Device\Tcpip_{A1B2C3D4-E5F6-4A1B-8C2D-000000000003}"
)
$tcpBinds = @(
    "\Device\{820C4568-D585-420E-94B3-AEA0E0DC3402}",
    "\Device\{A1B2C3D4-E5F6-4A1B-8C2D-000000000001}",
    "\Device\{A1B2C3D4-E5F6-4A1B-8C2D-000000000002}",
    "\Device\{A1B2C3D4-E5F6-4A1B-8C2D-000000000003}"
)
$tcpRoutes = @(
    "{820C4568-D585-420E-94B3-AEA0E0DC3402}",
    "{A1B2C3D4-E5F6-4A1B-8C2D-000000000001}",
    "{A1B2C3D4-E5F6-4A1B-8C2D-000000000002}",
    "{A1B2C3D4-E5F6-4A1B-8C2D-000000000003}"
)

$tcpLinkPath = "HKEY_LOCAL_MACHINE\ARM_SYS\ControlSet001\Services\Tcpip\Linkage"
Write-Host "Updating Tcpip Linkage..."
[Microsoft.Win32.Registry]::SetValue($tcpLinkPath, "Export", [string[]]$tcpExports, [Microsoft.Win32.RegistryValueKind]::MultiString)
[Microsoft.Win32.Registry]::SetValue($tcpLinkPath, "Bind", [string[]]$tcpBinds, [Microsoft.Win32.RegistryValueKind]::MultiString)
[Microsoft.Win32.Registry]::SetValue($tcpLinkPath, "Route", [string[]]$tcpRoutes, [Microsoft.Win32.RegistryValueKind]::MultiString)

# 3. Configure iScsiPrt wait for network
Write-Host "Updating iScsiPrt Parameters..."
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "WaitForNetworkAtBoot" /t REG_DWORD /d 1 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "DelayForNetworkAtBoot" /t REG_DWORD /d 5 /f

# 4. CriticalDeviceDatabase registration for all 3 NIC models
$cddEntries = @(
    @{ Key = "pci#ven_8086&dev_100e"; Service = "E1G60"; ClassGuid = "{4d36e972-e325-11ce-bfc1-08002be10318}" },
    @{ Key = "pci#ven_10ec&dev_8139"; Service = "RTL8023x64"; ClassGuid = "{4d36e972-e325-11ce-bfc1-08002be10318}" },
    @{ Key = "pci#ven_8086&dev_10d3"; Service = "e1iexpress"; ClassGuid = "{4d36e972-e325-11ce-bfc1-08002be10318}" }
)
foreach ($cdd in $cddEntries) {
    $cddPath = "HKLM\ARM_SYS\ControlSet001\Control\CriticalDeviceDatabase\$($cdd.Key)"
    Write-Host "Creating CriticalDeviceDatabase entry: $cddPath"
    & reg.exe add $cddPath /v "Service" /t REG_SZ /d $cdd.Service /f
    & reg.exe add $cddPath /v "ClassGUID" /t REG_SZ /d $cdd.ClassGuid /f
}

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg.exe unload HKLM\ARM_SYS
Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Write-Host "Network stack and CriticalDeviceDatabase successfully patched!"
Stop-Transcript
