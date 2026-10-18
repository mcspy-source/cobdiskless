# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "enable_debug_and_testsigning.log"
Start-Transcript -Path $logPath -Force

$vhd = 'G:\G\csmw10.VHD'
attrib -r $vhd
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadWrite | Out-Null
Start-Sleep -Seconds 2
$d = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $d.Number
$bcdPath = $null
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $cand = "$($p.DriveLetter):\Boot\BCD"
        if (Test-Path $cand) { $bcdPath = $cand; break }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Z -ErrorAction SilentlyContinue
        $cand = "Z:\Boot\BCD"
        if (Test-Path $cand) { $bcdPath = $cand; break }
    }
}

Write-Host "Found BCD: $bcdPath"
if ($bcdPath) {
    & bcdedit.exe /store $bcdPath /set "{default}" bootlog Yes
    & bcdedit.exe /store $bcdPath /set "{default}" sos Yes
    & bcdedit.exe /store $bcdPath /set "{default}" testsigning Yes
    & bcdedit.exe /store $bcdPath /set "{default}" nointegritychecks Yes
    & bcdedit.exe /store $bcdPath /set "{default}" debug Yes
    & bcdedit.exe /store $bcdPath /dbgsettings serial debugport:1 baudrate:115200
    Write-Host "Configured BCD with testsigning, nointegritychecks, and serial debugging!"
    & bcdedit.exe /store $bcdPath /enum "{default}"
    & bcdedit.exe /store $bcdPath /dbgsettings
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Stop-Transcript
