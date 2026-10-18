# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "inspect_inf_details.log"
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

Write-Host "Windows root: $vLetter"
if ($vLetter) {
    $repoDirs = @(
        "$vLetter\Windows\System32\DriverStore\FileRepository\net1ic64.inf_amd64_6d54118cb65f3bfe",
        "$vLetter\Windows\System32\DriverStore\FileRepository\nete1g3e.inf_amd64_af58b4e19562a3f9",
        "$vLetter\Windows\System32\DriverStore\FileRepository\netrtl64.inf_amd64_8e9c2368fe308df2"
    )

    foreach ($dir in $repoDirs) {
        Write-Host "=========================================="
        Write-Host "DIRECTORY: $dir"
        if (Test-Path $dir) {
            Get-ChildItem $dir | Select-Object Name, Length | Format-Table -AutoSize
            $inf = Get-ChildItem $dir -Filter "*.inf" | Select-Object -First 1
            if ($inf) {
                Write-Host "--- Service & CopyFiles in $($inf.Name) ---"
                Get-Content $inf.FullName | Select-String -Pattern "ServiceBinary|AddService|ServiceType|StartType|ErrorControl|LoadOrderGroup|CopyFiles" | Select-Object -First 30 | ForEach-Object { $_.Line.Trim() }
            }
        } else {
            Write-Host "Directory not found: $dir"
        }
    }
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Stop-Transcript
