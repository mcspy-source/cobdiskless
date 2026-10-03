$vhdPath = 'G:\csmw10.VHD'
$log = @()
$log += "Starting fixes on $vhdPath at $(Get-Date)"

$m = Mount-DiskImage -ImagePath $vhdPath -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32" }
$dl = $osPart.DriveLetter
$sysPath = "$dl`:\Windows\System32\config\SYSTEM"
$log += "OS Partition is $dl`: ($sysPath)"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

try {
    # 1. Clear FilterList and clean UpperBind on Net Adapter 0000
    $linkKey = "HKLM:\OFFLINE_SYS\ControlSet001\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}\0000\Linkage"
    if (Test-Path $linkKey) {
        Set-ItemProperty -Path $linkKey -Name "FilterList" -Value @() -Type MultiString
        Set-ItemProperty -Path $linkKey -Name "UpperBind" -Value @("Tcpip", "Tcpip6") -Type MultiString
        $log += "Adapter 0000 Linkage updated: FilterList cleared, UpperBind set to Tcpip, Tcpip6"
    } else {
        $log += "WARNING: LinkKey not found!"
    }

    # 2. DisableDHCPMediaSense in Tcpip\Parameters
    $tcpParams = "HKLM:\OFFLINE_SYS\ControlSet001\Services\Tcpip\Parameters"
    if (Test-Path $tcpParams) {
        Set-ItemProperty -Path $tcpParams -Name "DisableDHCPMediaSense" -Value 1 -Type DWord
        Set-ItemProperty -Path $tcpParams -Name "DisableTaskOffload" -Value 1 -Type DWord
        Set-ItemProperty -Path $tcpParams -Name "TcpMaxDataRetransmissions" -Value 10 -Type DWord
        $log += "Tcpip Parameters updated: DisableDHCPMediaSense=1, DisableTaskOffload=1, TcpMaxDataRetransmissions=10"
    }

    # 3. Configure Services\msiscsi
    $msKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\msiscsi"
    if (-not (Test-Path $msKey)) {
        New-Item -Path $msKey -Force | Out-Null
    }
    Set-ItemProperty -Path $msKey -Name "Start" -Value 0 -Type DWord
    Set-ItemProperty -Path $msKey -Name "BootFlags" -Value 1 -Type DWord
    Set-ItemProperty -Path $msKey -Name "Type" -Value 32 -Type DWord
    Set-ItemProperty -Path $msKey -Name "ErrorControl" -Value 1 -Type DWord
    Set-ItemProperty -Path $msKey -Name "Group" -Value "iSCSI" -Type String
    Set-ItemProperty -Path $msKey -Name "ImagePath" -Value "%systemroot%\system32\svchost.exe -k netsvcs -p" -Type ExpandString
    Set-ItemProperty -Path $msKey -Name "DisplayName" -Value "@%SystemRoot%\system32\iscsidsc.dll,-5000" -Type String
    Set-ItemProperty -Path $msKey -Name "Description" -Value "@%SystemRoot%\system32\iscsidsc.dll,-5001" -Type String
    Set-ItemProperty -Path $msKey -Name "ObjectName" -Value "LocalSystem" -Type String

    $msParams = "HKLM:\OFFLINE_SYS\ControlSet001\Services\msiscsi\Parameters"
    if (-not (Test-Path $msParams)) {
        New-Item -Path $msParams -Force | Out-Null
    }
    Set-ItemProperty -Path $msParams -Name "ServiceDll" -Value "%systemroot%\system32\iscsiexe.dll" -Type ExpandString
    Set-ItemProperty -Path $msParams -Name "ServiceDllUnloadOnStop" -Value 1 -Type DWord
    Set-ItemProperty -Path $msParams -Name "LinkDownTime" -Value 60 -Type DWord
    Set-ItemProperty -Path $msParams -Name "MaxRequestHoldTime" -Value 60 -Type DWord
    $log += "Services\msiscsi fully configured with Start=0, BootFlags=1"

    # 4. Configure Services\iScsiPrt
    $iscsiPrtKey = "HKLM:\OFFLINE_SYS\ControlSet001\Services\iScsiPrt"
    if (-not (Test-Path $iscsiPrtKey)) {
        New-Item -Path $iscsiPrtKey -Force | Out-Null
    }
    Set-ItemProperty -Path $iscsiPrtKey -Name "Start" -Value 0 -Type DWord
    Set-ItemProperty -Path $iscsiPrtKey -Name "BootFlags" -Value 1 -Type DWord
    Set-ItemProperty -Path $iscsiPrtKey -Name "Type" -Value 1 -Type DWord
    Set-ItemProperty -Path $iscsiPrtKey -Name "ErrorControl" -Value 1 -Type DWord
    Set-ItemProperty -Path $iscsiPrtKey -Name "Group" -Value "CCiSCSI" -Type String
    Set-ItemProperty -Path $iscsiPrtKey -Name "ImagePath" -Value "System32\drivers\msiscsi.sys" -Type ExpandString
    Set-ItemProperty -Path $iscsiPrtKey -Name "DisplayName" -Value "iScsiPort Driver" -Type String

    $iscsiPrtParams = "HKLM:\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Parameters"
    if (-not (Test-Path $iscsiPrtParams)) {
        New-Item -Path $iscsiPrtParams -Force | Out-Null
    }
    Set-ItemProperty -Path $iscsiPrtParams -Name "BusType" -Value 9 -Type DWord
    Set-ItemProperty -Path $iscsiPrtParams -Name "WaitForNetworkAtBoot" -Value 0 -Type DWord
    Set-ItemProperty -Path $iscsiPrtParams -Name "DelayForNetworkAtBoot" -Value 1 -Type DWord
    Set-ItemProperty -Path $iscsiPrtParams -Name "LinkDownTime" -Value 60 -Type DWord
    Set-ItemProperty -Path $iscsiPrtParams -Name "MaxRequestHoldTime" -Value 60 -Type DWord

    $iscsiPrtEnum = "HKLM:\OFFLINE_SYS\ControlSet001\Services\iScsiPrt\Enum"
    if (-not (Test-Path $iscsiPrtEnum)) {
        New-Item -Path $iscsiPrtEnum -Force | Out-Null
    }
    Set-ItemProperty -Path $iscsiPrtEnum -Name "0" -Value "ROOT\ISCSIPRT\0000" -Type String
    Set-ItemProperty -Path $iscsiPrtEnum -Name "Count" -Value 1 -Type DWord
    Set-ItemProperty -Path $iscsiPrtEnum -Name "NextInstance" -Value 1 -Type DWord
    $log += "Services\iScsiPrt fully configured"

    # 5. Configure Enum\ROOT\ISCSIPRT\0000 with REG_MULTI_SZ HardwareID
    $enumKey = "HKLM:\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT\0000"
    if (-not (Test-Path $enumKey)) {
        New-Item -Path $enumKey -Force | Out-Null
    }
    Set-ItemProperty -Path $enumKey -Name "HardwareID" -Value @("ROOT\ISCSIPRT") -Type MultiString
    Set-ItemProperty -Path $enumKey -Name "ClassGUID" -Value "{4d36e97b-e325-11ce-bfc1-08002be10318}" -Type String
    Set-ItemProperty -Path $enumKey -Name "Service" -Value "iScsiPrt" -Type String
    Set-ItemProperty -Path $enumKey -Name "Driver" -Value "{4d36e97b-e325-11ce-bfc1-08002be10318}\0000" -Type String
    Set-ItemProperty -Path $enumKey -Name "DeviceDesc" -Value "Microsoft iSCSI Initiator" -Type String
    Set-ItemProperty -Path $enumKey -Name "Mfg" -Value "Microsoft" -Type String
    Set-ItemProperty -Path $enumKey -Name "ConfigFlags" -Value 0 -Type DWord
    $log += "Enum\ROOT\ISCSIPRT\0000 configured with MultiString HardwareID"

    # 6. Configure SCSI Class {4d36e97b-e325-11ce-bfc1-08002be10318}\0000
    $scsi0 = "HKLM:\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}\0000"
    if (-not (Test-Path $scsi0)) {
        New-Item -Path $scsi0 -Force | Out-Null
    }
    Set-ItemProperty -Path $scsi0 -Name "DriverDesc" -Value "Microsoft iSCSI Initiator" -Type String
    Set-ItemProperty -Path $scsi0 -Name "ProviderName" -Value "Microsoft" -Type String
    Set-ItemProperty -Path $scsi0 -Name "MatchingDeviceId" -Value "ROOT\ISCSIPRT" -Type String
    Set-ItemProperty -Path $scsi0 -Name "InfPath" -Value "iscsi.inf" -Type String
    Set-ItemProperty -Path $scsi0 -Name "InfSection" -Value "iScsiPort_Install_Control" -Type String

    $scsi0Params = "HKLM:\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}\0000\Parameters"
    if (-not (Test-Path $scsi0Params)) {
        New-Item -Path $scsi0Params -Force | Out-Null
    }
    Set-ItemProperty -Path $scsi0Params -Name "TCPConnectTime" -Value 15 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "TCPDisconnectTime" -Value 15 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "WMIRequestTimeout" -Value 30 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "DelayBetweenReconnect" -Value 5 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "MaxPendingRequests" -Value 255 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "MaxConnectionRetries" -Value 0xFFFFFFFF -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "MaxRequestHoldTime" -Value 60 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "LinkDownTime" -Value 60 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "ImmediateData" -Value 1 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "ErrorRecoveryLevel" -Value 2 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "PortalRetryCount" -Value 5 -Type DWord
    Set-ItemProperty -Path $scsi0Params -Name "NetworkReadyRetryCount" -Value 10 -Type DWord
    $log += "SCSI Class 0000 configured"

    # 7. Add CriticalDeviceDatabase entry for Root#iScsiPrt
    $cddbIscsi = "HKLM:\OFFLINE_SYS\ControlSet001\Control\CriticalDeviceDatabase\root#iscsiprt"
    if (-not (Test-Path $cddbIscsi)) {
        New-Item -Path $cddbIscsi -Force | Out-Null
    }
    Set-ItemProperty -Path $cddbIscsi -Name "ClassGUID" -Value "{4d36e97b-e325-11ce-bfc1-08002be10318}" -Type String
    Set-ItemProperty -Path $cddbIscsi -Name "Service" -Value "iScsiPrt" -Type String
    $log += "CDDB root#iscsiprt configured"

    $log += "SUCCESS: All registry modifications applied successfully!"
}
catch {
    $log += "ERROR: $($_.Exception.Message)"
}
finally {
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg unload HKLM\OFFLINE_SYS
    Start-Sleep -Milliseconds 500

    # Truncate and wipe transaction logs
    $cfgDir = "$dl`:\Windows\System32\config"
    foreach ($logFile in @("SYSTEM.LOG1", "SYSTEM.LOG2", "SYSTEM.LOG")) {
        $logPath = Join-Path $cfgDir $logFile
        if (Test-Path $logPath) {
            Set-Content -Path $logPath -Value @() -NoNewline -Force
            Remove-Item -Path $logPath -Force -ErrorAction SilentlyContinue
            $log += "Cleaned log: $logFile"
        }
    }

    Dismount-DiskImage -ImagePath $vhdPath
    $log += "Dismounted VHD."
}

$log | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\apply_critical_fixes_result.txt" -Encoding utf8
