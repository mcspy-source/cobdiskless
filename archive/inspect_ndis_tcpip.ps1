# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "inspect_ndis_tcpip.log"
Start-Transcript -Path $logPath -Force

$vhd = "G:\G\csmw10.VHD"
attrib -r $vhd
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly | Out-Null
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
if ($vLetter) {
    & reg.exe load HKLM\ARM_SYS "$vLetter\Windows\System32\config\SYSTEM"

    Write-Host "=== NDIS Linkage ==="
    & reg.exe query "HKLM\ARM_SYS\ControlSet001\Services\NDIS\Linkage" /s 2>$null
    Write-Host "=== Tcpip Linkage ==="
    & reg.exe query "HKLM\ARM_SYS\ControlSet001\Services\Tcpip\Linkage" /s 2>$null
    Write-Host "=== Tcpip Interfaces ==="
    & reg.exe query "HKLM\ARM_SYS\ControlSet001\Services\Tcpip\Parameters\Interfaces" /s 2>$null
    Write-Host "=== iScsiPrt Parameters ==="
    & reg.exe query "HKLM\ARM_SYS\ControlSet001\Services\iScsiPrt\Parameters" /s 2>$null
    Write-Host "=== Net Class ==="
    & reg.exe query "HKLM\ARM_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}" 2>$null

    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg.exe unload HKLM\ARM_SYS
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Stop-Transcript
