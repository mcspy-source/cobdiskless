# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "inspect_vhd_bcd.log"
Start-Transcript -Path $logPath -Force

$vhd = 'G:\G\csmw10.VHD'
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly | Out-Null
Start-Sleep -Seconds 2
$d = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $d.Number
$bcdPath = $null
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $cand = "$($p.DriveLetter):\Boot\BCD"
        if (Test-Path $cand) { $bcdPath = $cand; break }
        $cand2 = "$($p.DriveLetter):\EFI\Microsoft\Boot\BCD"
        if (Test-Path $cand2) { $bcdPath = $cand2; break }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Z -ErrorAction SilentlyContinue
        $cand = "Z:\Boot\BCD"
        if (Test-Path $cand) { $bcdPath = $cand; break }
        $cand2 = "Z:\EFI\Microsoft\Boot\BCD"
        if (Test-Path $cand2) { $bcdPath = $cand2; break }
    }
}

Write-Host "BCD Path: $bcdPath"
if ($bcdPath) {
    & bcdedit.exe /store $bcdPath /enum all
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
Stop-Transcript
