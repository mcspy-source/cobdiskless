# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "arm_multi_nic.log"
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

$driversDir = "$vLetter\Windows\System32\drivers"
$fileRepo = "$vLetter\Windows\System32\DriverStore\FileRepository"

# Copy driver binaries
$copies = @(
    @{ Src = "$fileRepo\nete1g3e.inf_amd64_af58b4e19562a3f9\E1G6032E.sys"; Dst = "$driversDir\E1G6032E.sys" },
    @{ Src = "$fileRepo\net1ic64.inf_amd64_6d54118cb65f3bfe\e1i63x64.sys"; Dst = "$driversDir\e1i63x64.sys" },
    @{ Src = "$fileRepo\netrtl64.inf_amd64_8e9c2368fe308df2\Rtnic64.sys"; Dst = "$driversDir\Rtnic64.sys" }
)

foreach ($c in $copies) {
    Write-Host "Copying $($c.Src) -> $($c.Dst)"
    Copy-Item -Path $c.Src -Destination $c.Dst -Force
    if (Test-Path $c.Dst) {
        Write-Host "  Success: $((Get-Item $c.Dst).Length) bytes"
    } else {
        Write-Error "  Failed to copy $($c.Dst)"
    }
}

# Load SYSTEM hive
Write-Host "Loading SYSTEM hive to HKLM\ARM_SYS..."
& reg.exe load HKLM\ARM_SYS "$vLetter\Windows\System32\config\SYSTEM"

# Function to configure service
function Set-DriverService {
    param(
        [string]$ServiceName,
        [string]$ImagePath,
        [string]$DisplayName,
        [string]$Group = "NDIS"
    )
    $svcPath = "HKLM\ARM_SYS\ControlSet001\Services\$ServiceName"
    Write-Host "Configuring service: $svcPath"
    & reg.exe add $svcPath /v "Type" /t REG_DWORD /d 1 /f
    & reg.exe add $svcPath /v "Start" /t REG_DWORD /d 0 /f
    & reg.exe add $svcPath /v "ErrorControl" /t REG_DWORD /d 1 /f
    & reg.exe add $svcPath /v "ImagePath" /t REG_EXPAND_SZ /d $ImagePath /f
    & reg.exe add $svcPath /v "Group" /t REG_SZ /d $Group /f
    & reg.exe add $svcPath /v "DisplayName" /t REG_SZ /d $DisplayName /f
    & reg.exe add $svcPath /v "BootFlags" /t REG_DWORD /d 1 /f

    # Delete StartOverride if exists
    & reg.exe delete "$svcPath\StartOverride" /f 2>$null
}

# Function to configure CriticalDeviceDatabase entry
function Set-CDDBEntry {
    param(
        [string]$HardwareId,
        [string]$Service,
        [string]$ClassGUID = "{4d36e972-e325-11ce-bfc1-08002be10318}"
    )
    $cddbPath = "HKLM\ARM_SYS\ControlSet001\Control\CriticalDeviceDatabase\$HardwareId"
    Write-Host "Configuring CDDB: $cddbPath -> Service: $Service"
    & reg.exe add $cddbPath /v "ClassGUID" /t REG_SZ /d $ClassGUID /f
    & reg.exe add $cddbPath /v "Service" /t REG_SZ /d $Service /f
}

# 1. E1G60 (Intel e1000 - 8086:100E)
Set-DriverService -ServiceName "E1G60" -ImagePath "System32\drivers\E1G6032E.sys" -DisplayName "Intel PRO/1000 PCI Adapter Driver"
Set-CDDBEntry -HardwareId "pci#ven_8086&dev_100e" -Service "E1G60"
Set-CDDBEntry -HardwareId "pci#ven_8086&dev_100e&subsys_001e8086" -Service "E1G60"
Set-CDDBEntry -HardwareId "pci#ven_8086&dev_100e&subsys_002e8086" -Service "E1G60"

# 2. e1iexpress (Intel e1000e - 8086:10D3)
Set-DriverService -ServiceName "e1iexpress" -ImagePath "System32\drivers\e1i63x64.sys" -DisplayName "Intel Gigabit Network Connection Driver"
Set-CDDBEntry -HardwareId "pci#ven_8086&dev_10d3" -Service "e1iexpress"
Set-CDDBEntry -HardwareId "pci#ven_8086&dev_10d3&subsys_a01f8086" -Service "e1iexpress"
Set-CDDBEntry -HardwareId "pci#ven_8086&dev_10d3&subsys_00018086" -Service "e1iexpress"

# 3. RTL8023x64 (Realtek rtl8139 - 10EC:8139)
Set-DriverService -ServiceName "RTL8023x64" -ImagePath "System32\drivers\Rtnic64.sys" -DisplayName "Realtek RTL8139 Family Fast Ethernet Driver"
Set-CDDBEntry -HardwareId "pci#ven_10ec&dev_8139" -Service "RTL8023x64"
Set-CDDBEntry -HardwareId "pci#ven_10ec&dev_8139&rev_10" -Service "RTL8023x64"
Set-CDDBEntry -HardwareId "pci#ven_10ec&dev_8139&rev_20" -Service "RTL8023x64"

# 4. Rt640x64 (Realtek rtl8168 - 10EC:8168) - already present, verify boot-start
Set-DriverService -ServiceName "Rt640x64" -ImagePath "System32\drivers\rt640x64.sys" -DisplayName "Realtek PCIe GbE Family Controller"
Set-CDDBEntry -HardwareId "pci#ven_10ec&dev_8168" -Service "Rt640x64"
Set-CDDBEntry -HardwareId "pci#ven_10ec&dev_8168&rev_01" -Service "Rt640x64"

# Ensure ndis and Tcpip are boot-start with BootFlags
Write-Host "Arming ndis and Tcpip..."
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\ndis" /v "Start" /t REG_DWORD /d 0 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\ndis" /v "BootFlags" /t REG_DWORD /d 1 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\ndis" /v "Group" /t REG_SZ /d "NDIS Wrapper" /f

& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\Tcpip" /v "Start" /t REG_DWORD /d 0 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\Tcpip" /v "BootFlags" /t REG_DWORD /d 1 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\Tcpip" /v "Group" /t REG_SZ /d "PNP_TDI" /f

# Ensure iScsiPrt is boot-start kernel driver in CCiSCSI group
Write-Host "Arming iScsiPrt driver..."
Set-DriverService -ServiceName "iScsiPrt" -ImagePath "System32\drivers\msiscsi.sys" -DisplayName "Microsoft iSCSI Initiator Driver" -Group "CCiSCSI"
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "BusType" /t REG_DWORD /d 9 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "WaitForNetworkAtBoot" /t REG_DWORD /d 0 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Parameters" /v "DelayForNetworkAtBoot" /t REG_DWORD /d 1 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Enum" /v "0" /t REG_SZ /d "ROOT\ISCSIPRT\0000" /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Enum" /v "Count" /t REG_DWORD /d 1 /f
& reg.exe add "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Enum" /v "NextInstance" /t REG_DWORD /d 1 /f

# CDDB: Map root#iscsiprt to iScsiPrt service
Set-CDDBEntry -HardwareId "root#iscsiprt" -Service "iScsiPrt" -ClassGUID "{4d36e97b-e325-11ce-bfc1-08002be10318}"

# Restore msiscsi as user-mode service
Write-Host "Restoring msiscsi user-mode service..."
$msiscsiPath = "HKLM\ARM_SYS\ControlSet001\Services\msiscsi"
& reg.exe add $msiscsiPath /v "Type" /t REG_DWORD /d 32 /f
& reg.exe add $msiscsiPath /v "Start" /t REG_DWORD /d 3 /f
& reg.exe add $msiscsiPath /v "ErrorControl" /t REG_DWORD /d 1 /f
& reg.exe add $msiscsiPath /v "ImagePath" /t REG_EXPAND_SZ /d "%systemroot%\system32\svchost.exe -k netsvcs -p" /f
& reg.exe add $msiscsiPath /v "Group" /t REG_SZ /d "iSCSI" /f
& reg.exe add $msiscsiPath /v "ObjectName" /t REG_SZ /d "LocalSystem" /f
& reg.exe add "$msiscsiPath\Parameters" /v "ServiceDll" /t REG_EXPAND_SZ /d "%systemroot%\system32\iscsiexe.dll" /f
& reg.exe add "$msiscsiPath\Parameters" /v "ServiceDllUnloadOnStop" /t REG_DWORD /d 1 /f

# Set ServiceGroupOrder to ensure network stack precedes storage in boot
Write-Host "Configuring ServiceGroupOrder for iSCSI network boot..."
$sgoList = @(
    "System Reserved",
    "EMS",
    "WdfLoadGroup",
    "Boot Bus Extender",
    "System Bus Extender",
    "NDIS Wrapper",
    "NDIS",
    "Base",
    "PNP_TDI",
    "CCiSCSI",
    "SCSI miniport",
    "Port",
    "Primary Disk",
    "SCSI Class",
    "SCSI CDROM Class",
    "FSFilter Infrastructure",
    "FSFilter System",
    "FSFilter Bottom",
    "FSFilter Copy Protection",
    "FSFilter Security Enhancer",
    "FSFilter Open File",
    "FSFilter Physical Quota Management",
    "FSFilter Virtualization",
    "FSFilter Encryption",
    "FSFilter Compression",
    "FSFilter Imaging",
    "FSFilter HSM",
    "FSFilter Cluster File System",
    "FSFilter System Recovery",
    "FSFilter Quota Management",
    "FSFilter Content Screener",
    "FSFilter Continuous Backup",
    "FSFilter Replication",
    "FSFilter Anti-Virus",
    "FSFilter Undelete",
    "FSFilter Activity Monitor",
    "FSFilter Top",
    "Filter",
    "Boot File System",
    "Pointer Port",
    "Keyboard Port",
    "Pointer Class",
    "Keyboard Class",
    "Video Init",
    "Video",
    "Video Save",
    "File System",
    "Streams Drivers",
    "COM Infrastructure",
    "Event Log",
    "PerceptionGroup",
    "ProfSvc_Group",
    "AudioGroup",
    "UIGroup",
    "MS_WindowsLocalValidation",
    "PlugPlay",
    "Cryptography",
    "TDI",
    "iSCSI",
    "NetBIOSGroup",
    "ShellSvcGroup",
    "SchedulerGroup",
    "SpoolerGroup",
    "SmartCardGroup",
    "NetworkProvider",
    "MS_WindowsRemoteValidation",
    "NetDDEGroup",
    "Parallel arbitrator",
    "Extended Base",
    "PCI Configuration",
    "MS Transactions"
)
[Microsoft.Win32.Registry]::SetValue("HKEY_LOCAL_MACHINE\ARM_SYS\ControlSet001\Control\ServiceGroupOrder", "List", [string[]]$sgoList, [Microsoft.Win32.RegistryValueKind]::MultiString)
Write-Host "ServiceGroupOrder List successfully updated via .NET!"

Write-Host "Unloading SYSTEM hive..."
[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg.exe unload HKLM\ARM_SYS

Write-Host "Dismounting VHD..."
Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Write-Host "Multi-NIC Driver and CDDB Arming Completed!"
Stop-Transcript
