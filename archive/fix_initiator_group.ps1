# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "fix_initiator_group.log"
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

if ($vLetter) {
    & reg.exe load HKLM\FIX_SYS "$vLetter\Windows\System32\config\SYSTEM"

    Write-Host "Fixing msiscsi Group -> iSCSI"
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\msiscsi" /v "Group" /t REG_SZ /d "iSCSI" /f

    Write-Host "Verifying iScsiPrt Group and ImagePath"
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt" /v "Start" /t REG_DWORD /d 0 /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt" /v "BootFlags" /t REG_DWORD /d 1 /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt" /v "Group" /t REG_SZ /d "CCiSCSI" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Services\iScsiPrt" /v "ImagePath" /t REG_EXPAND_SZ /d "System32\drivers\msiscsi.sys" /f

    Write-Host "Fixing CDDB root#iscsiprt -> Service iScsiPrt"
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\CriticalDeviceDatabase\root#iscsiprt" /v "Service" /t REG_SZ /d "iScsiPrt" /f
    & reg.exe add "HKLM\FIX_SYS\ControlSet001\Control\CriticalDeviceDatabase\root#iscsiprt" /v "ClassGUID" /t REG_SZ /d "{4d36e97b-e325-11ce-bfc1-08002be10318}" /f

    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg.exe unload HKLM\FIX_SYS
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Write-Host "Initiator group and CDDB fix complete!"
Stop-Transcript
