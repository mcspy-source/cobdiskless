# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "read_pnp_keys.log"
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

if ($vLetter) {
    & reg.exe load HKLM\CHECK_SYS "$vLetter\Windows\System32\config\SYSTEM"

    Write-Host "=== CriticalDeviceDatabase entries ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\CriticalDeviceDatabase" /s | Select-String "8086|10ec|iscsi" -Context 1, 3

    Write-Host "=== Control\Pnp entries ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Pnp" 2>$null

    Write-Host "=== Services\msiscsi ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\msiscsi"

    Write-Host "=== Services\E1G60 ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\E1G60"

    Write-Host "=== Services\e1iexpress ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\e1iexpress"

    Write-Host "=== Services\RTL8023x64 ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\RTL8023x64"

    Write-Host "=== Services\Rt640x64 ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\Rt640x64"

    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg.exe unload HKLM\CHECK_SYS
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Stop-Transcript
