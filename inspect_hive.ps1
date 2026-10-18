try {
    $img = Mount-DiskImage -ImagePath "G:\G\csmw10.VHD" -PassThru
    $disk = Get-DiskImage -ImagePath "G:\G\csmw10.VHD" | Get-Disk
    $parts = Get-Partition -DiskNumber $disk.Number
    $volPart = $parts | Where-Object { $_.DriveLetter }
    if (-not $volPart) {
        $volPart = $parts | Where-Object { $_.Size -gt 10GB } | Select-Object -First 1
        Set-Partition -DiskNumber $disk.Number -PartitionNumber $volPart.PartitionNumber -NewDriveLetter "Z"
        $letter = "Z"
    } else {
        $letter = $volPart.DriveLetter
    }
    $sysPath = "${letter}:\Windows\System32\config\SYSTEM"
    & reg.exe load HKLM\CHECK_SYS $sysPath 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0000" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\rt640x64" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\rt640x64\Enum" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\msiscsi" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\iScsiPrt" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\Tcpip" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\ndis" 2>&1 | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\CriticalDeviceDatabase" 2>&1 | Select-String -Pattern "10ec|8168|rt64|iscsi" | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
    & reg.exe unload HKLM\CHECK_SYS
    Dismount-DiskImage -ImagePath "G:\G\csmw10.VHD" | Out-Null
} catch {
    $_ | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\class_net_query.txt" -Append -Encoding ascii
}
