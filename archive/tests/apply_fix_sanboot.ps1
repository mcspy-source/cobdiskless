$ErrorActionPreference = 'Stop'
$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== APPLYING SANBOOT & SERVICEGROUPORDER FIXES TO $sysPath ==="

# 1. Update ServiceGroupOrder\List to ensure NDIS and CCiSCSI load before SCSI miniport
$targetOrder = @(
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
$multiSzVal = [string]::Join("`0", $targetOrder) + "`0`0"
# Use PowerShell registry provider to write REG_MULTI_SZ reliably
Set-ItemProperty -Path "HKLM:\OFFLINE_SYS\ControlSet001\Control\ServiceGroupOrder" -Name "List" -Value $targetOrder
$out += "Updated ServiceGroupOrder\List successfully."

# 2. Sanitize Tcpip\Parameters\Interfaces
$ifaceRoot = "HKLM:\OFFLINE_SYS\ControlSet001\Services\Tcpip\Parameters\Interfaces"
if (Test-Path $ifaceRoot) {
    Get-ChildItem -Path $ifaceRoot | ForEach-Object {
        $keyPath = $_.PSPath
        Set-ItemProperty -Path $keyPath -Name "DhcpIPAddress" -Value "0.0.0.0" -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $keyPath -Name "DhcpServer" -Value "255.255.255.255" -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $keyPath -Name "EnableDHCP" -Value 1 -ErrorAction SilentlyContinue
        
        $propsToRemove = @(
            "DhcpDefaultGateway",
            "DhcpGatewayHardware",
            "DhcpGatewayHardwareCount",
            "DhcpInterfaceOptions",
            "DhcpSubnetMaskOpt",
            "Lease",
            "LeaseObtainedTime",
            "LeaseTerminatesTime",
            "T1",
            "T2"
        )
        foreach ($prop in $propsToRemove) {
            Remove-ItemProperty -Path $keyPath -Name $prop -ErrorAction SilentlyContinue
        }
        $out += "Sanitized interface: $($_.PSChildName)"
    }
}

# 3. Disable Session Manager BootExecute autochk
Set-ItemProperty -Path "HKLM:\OFFLINE_SYS\ControlSet001\Control\Session Manager" -Name "BootExecute" -Value @()
$out += "Cleared BootExecute to prevent chkdsk hang."

# 4. Verify iScsiPrt and rt640x64 settings
$scsiKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\iScsiPrt"
Set-ItemProperty -Path $scsiKey -Name "Start" -Value 0
Set-ItemProperty -Path $scsiKey -Name "Group" -Value "CCiSCSI"
Set-ItemProperty -Path $scsiKey -Name "BootFlags" -Value 1

$rtKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\rt640x64"
if (Test-Path $rtKey) {
    Set-ItemProperty -Path $rtKey -Name "Start" -Value 0
    Set-ItemProperty -Path $rtKey -Name "Group" -Value "NDIS"
    Set-ItemProperty -Path $rtKey -Name "BootFlags" -Value 1
}

# 5. Verify verification dump
$out += "=== VERIFY SERVICEGROUPORDER ==="
$out += (Get-ItemProperty -Path "HKLM:\OFFLINE_SYS\ControlSet001\Control\ServiceGroupOrder" -Name "List").List -join ", "
$out += "=== VERIFY BOOTEXECUTE ==="
$out += "BootExecute count: " + ((Get-ItemProperty -Path "HKLM:\OFFLINE_SYS\ControlSet001\Control\Session Manager" -Name "BootExecute").BootExecute.Count)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS

# Clean up dirty logs
$cfgDir = Split-Path $sysPath
foreach ($log in @("SYSTEM.LOG", "SYSTEM.LOG1", "SYSTEM.LOG2")) {
    $lp = Join-Path $cfgDir $log
    if (Test-Path $lp) {
        Remove-Item -Path $lp -Force -ErrorAction SilentlyContinue
        $out += "Removed dirty log: $log"
    }
}

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\apply_fix_result.txt" -Encoding utf8
