# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "check_sys_files.log"
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
        $testPath = "$($p.DriveLetter):\Windows\System32\drivers\msiscsi.sys"
        if (Test-Path $testPath) { $vLetter = "$($p.DriveLetter):"; break }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Y -ErrorAction SilentlyContinue
        if (Test-Path "Y:\Windows\System32\drivers\msiscsi.sys") { $vLetter = "Y:"; break }
    }
}

Write-Host "Mounted Windows root: $vLetter"
if ($vLetter) {
    $files = @(
        "$vLetter\Windows\System32\drivers\msiscsi.sys",
        "$vLetter\Windows\System32\drivers\E1G6032E.sys",
        "$vLetter\Windows\System32\drivers\Rtnic64.sys",
        "$vLetter\Windows\System32\drivers\e1i63x64.sys",
        "$vLetter\Windows\System32\drivers\ndis.sys",
        "$vLetter\Windows\System32\drivers\tcpip.sys",
        "$vLetter\Windows\System32\drivers\Wdf01000.sys"
    )
    foreach ($f in $files) {
        if (Test-Path $f) {
            $item = Get-Item $f
            Write-Host "[EXISTS] $f ($($item.Length) bytes, Version: $($item.VersionInfo.FileVersion))"
        } else {
            Write-Host "[MISSING] $f" -ForegroundColor Red
        }
    }
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Stop-Transcript
