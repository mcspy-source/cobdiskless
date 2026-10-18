# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "inject_pnp_instances.log"
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

# Helper to inject Network Class entry
function Add-NetClassInstance {
    param(
        [string]$Index, # e.g. "0002"
        [string]$DriverDesc,
        [string]$MatchingDeviceId,
        [string]$DeviceInstanceID,
        [string]$NetCfgGuid
    )
    $classPath = "HKLM\ARM_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\$Index"
    Write-Host "Creating Net Class: $classPath"
    & reg.exe add $classPath /v "DriverDesc" /t REG_SZ /d $DriverDesc /f
    & reg.exe add $classPath /v "ProviderName" /t REG_SZ /d "Microsoft" /f
    & reg.exe add $classPath /v "MatchingDeviceId" /t REG_SZ /d $MatchingDeviceId /f
    & reg.exe add $classPath /v "DeviceInstanceID" /t REG_SZ /d $DeviceInstanceID /f
    & reg.exe add $classPath /v "NetCfgInstanceId" /t REG_SZ /d $NetCfgGuid /f
    & reg.exe add $classPath /v "Characteristics" /t REG_DWORD /d 132 /f
    & reg.exe add $classPath /v "BusType" /t REG_SZ /d "5" /f
    & reg.exe add "$classPath\Linkage" /v "RootDevice" /t REG_MULTI_SZ /d $NetCfgGuid /f
    & reg.exe add "$classPath\Linkage" /v "UpperBind" /t REG_MULTI_SZ /d "Tcpip\0Tcpip6" /f
}

# Helper to inject Enum\PCI device instance
function Add-PciEnumInstance {
    param(
        [string]$DeviceKey, # e.g. "VEN_8086&DEV_100E&SUBSYS_001E8086&REV_00"
        [string]$InstanceId, # e.g. "0018"
        [string]$DeviceDesc,
        [string]$Service,
        [string[]]$HwIds,
        [string[]]$CompatIds,
        [string]$DriverIndex # e.g. "0002"
    )
    $enumPath = "HKLM\ARM_SYS\ControlSet001\Enum\PCI\$DeviceKey\$InstanceId"
    Write-Host "Creating PCI Enum Instance: $enumPath"
    & reg.exe add $enumPath /v "DeviceDesc" /t REG_SZ /d $DeviceDesc /f
    & reg.exe add $enumPath /v "Service" /t REG_SZ /d $Service /f
    & reg.exe add $enumPath /v "ClassGUID" /t REG_SZ /d "{4d36e972-e325-11ce-bfc1-08002be10318}" /f
    & reg.exe add $enumPath /v "Driver" /t REG_SZ /d "{4d36e972-e325-11ce-bfc1-08002be10318}\$DriverIndex" /f
    & reg.exe add $enumPath /v "ConfigFlags" /t REG_DWORD /d 0 /f
    & reg.exe add $enumPath /v "Capabilities" /t REG_DWORD /d 16 /f
    & reg.exe add $enumPath /v "ContainerID" /t REG_SZ /d "{00000000-0000-0000-ffff-ffffffffffff}" /f
    & reg.exe add $enumPath /v "LocationInformation" /t REG_SZ /d "PCI bus 0, device 3, function 0" /f

    # Multi-string hardware & compat IDs via .NET
    $hwKey = "HKEY_LOCAL_MACHINE\ARM_SYS\ControlSet001\Enum\PCI\$DeviceKey\$InstanceId"
    [Microsoft.Win32.Registry]::SetValue($hwKey, "HardwareID", [string[]]$HwIds, [Microsoft.Win32.RegistryValueKind]::MultiString)
    [Microsoft.Win32.Registry]::SetValue($hwKey, "CompatibleIDs", [string[]]$CompatIds, [Microsoft.Win32.RegistryValueKind]::MultiString)
}

# 1. VM 1: Intel e1000 (8086:100E)
Add-NetClassInstance -Index "0002" `
    -DriverDesc "Intel PRO/1000 MT Network Connection" `
    -MatchingDeviceId "PCI\VEN_8086&DEV_100E" `
    -DeviceInstanceID "PCI\VEN_8086&DEV_100E&SUBSYS_001E8086&REV_00\0018" `
    -NetCfgGuid "{A1B2C3D4-E5F6-4A1B-8C2D-000000000001}"

Add-PciEnumInstance -DeviceKey "VEN_8086&DEV_100E&SUBSYS_001E8086&REV_00" `
    -InstanceId "0018" `
    -DeviceDesc "Intel PRO/1000 MT Network Connection" `
    -Service "E1G60" `
    -HwIds @("PCI\VEN_8086&DEV_100E&SUBSYS_001E8086&REV_00", "PCI\VEN_8086&DEV_100E&SUBSYS_001E8086", "PCI\VEN_8086&DEV_100E&CC_020000", "PCI\VEN_8086&DEV_100E&CC_0200") `
    -CompatIds @("PCI\VEN_8086&DEV_100E&REV_00", "PCI\VEN_8086&DEV_100E", "PCI\VEN_8086&CC_020000", "PCI\VEN_8086&CC_0200", "PCI\VEN_8086", "PCI\CC_020000", "PCI\CC_0200") `
    -DriverIndex "0002"

# 2. VM 2: Realtek rtl8139 (10EC:8139)
Add-NetClassInstance -Index "0003" `
    -DriverDesc "Realtek RTL8139 Family Fast Ethernet Controller" `
    -MatchingDeviceId "PCI\VEN_10EC&DEV_8139" `
    -DeviceInstanceID "PCI\VEN_10EC&DEV_8139&SUBSYS_00000000&REV_20\0018" `
    -NetCfgGuid "{A1B2C3D4-E5F6-4A1B-8C2D-000000000002}"

Add-PciEnumInstance -DeviceKey "VEN_10EC&DEV_8139&SUBSYS_00000000&REV_20" `
    -InstanceId "0018" `
    -DeviceDesc "Realtek RTL8139 Family Fast Ethernet Controller" `
    -Service "RTL8023x64" `
    -HwIds @("PCI\VEN_10EC&DEV_8139&SUBSYS_00000000&REV_20", "PCI\VEN_10EC&DEV_8139&SUBSYS_00000000", "PCI\VEN_10EC&DEV_8139&CC_020000", "PCI\VEN_10EC&DEV_8139&CC_0200") `
    -CompatIds @("PCI\VEN_10EC&DEV_8139&REV_20", "PCI\VEN_10EC&DEV_8139", "PCI\VEN_10EC&CC_020000", "PCI\VEN_10EC&CC_0200", "PCI\VEN_10EC", "PCI\CC_020000", "PCI\CC_0200") `
    -DriverIndex "0003"

# 3. VM 3: Intel e1000e (8086:10D3)
Add-NetClassInstance -Index "0004" `
    -DriverDesc "Intel 82574L Gigabit Network Connection" `
    -MatchingDeviceId "PCI\VEN_8086&DEV_10D3" `
    -DeviceInstanceID "PCI\VEN_8086&DEV_10D3&SUBSYS_00000000&REV_00\0018" `
    -NetCfgGuid "{A1B2C3D4-E5F6-4A1B-8C2D-000000000003}"

Add-PciEnumInstance -DeviceKey "VEN_8086&DEV_10D3&SUBSYS_00000000&REV_00" `
    -InstanceId "0018" `
    -DeviceDesc "Intel 82574L Gigabit Network Connection" `
    -Service "e1iexpress" `
    -HwIds @("PCI\VEN_8086&DEV_10D3&SUBSYS_00000000&REV_00", "PCI\VEN_8086&DEV_10D3&SUBSYS_00000000", "PCI\VEN_8086&DEV_10D3&CC_020000", "PCI\VEN_8086&DEV_10D3&CC_0200") `
    -CompatIds @("PCI\VEN_8086&DEV_10D3&REV_00", "PCI\VEN_8086&DEV_10D3", "PCI\VEN_8086&CC_020000", "PCI\VEN_8086&CC_0200", "PCI\VEN_8086", "PCI\CC_020000", "PCI\CC_0200") `
    -DriverIndex "0004"

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg.exe unload HKLM\ARM_SYS
Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Write-Host "All PnP device instances successfully pre-populated in master VHD!"
Stop-Transcript
