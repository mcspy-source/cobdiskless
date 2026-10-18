$vhd = "G:\G\csmw10.VHD"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\cluster_lookup.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\cluster_lookup.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\cluster_lookup_run.ps1"
$supContent = @"
`$vhd = '$vhd'
`$logFile = '$logFile'
`$marker = '$marker'

Start-Transcript -Path `$logFile -Force
Mount-DiskImage -ImagePath `$vhd -Access ReadOnly | Out-Null
Start-Sleep -Seconds 1
`$d = Get-DiskImage -ImagePath `$vhd | Get-Disk
`$parts = Get-Partition -DiskNumber `$d.Number | Where-Object { `$_.Type -ne 'Reserved' }
`$vLetter = `$null
foreach (`$p in `$parts) {
    if (`$p.DriveLetter) {
        `$testPath = "`$(`$p.DriveLetter):\Windows\System32\config\SYSTEM"
        if (Test-Path `$testPath) { `$vLetter = "`$(`$p.DriveLetter):"; break }
    }
}

Write-Host "VHD Partition: `$vLetter"
if (`$vLetter) {
    # Relative cluster is 316279
    # Use fsutil to query cluster
    Write-Host "Querying cluster 316279 on `$vLetter..."
    & fsutil file queryFileNameById `$vLetter 0x0000000000000000 2>`$null
    # Or query by cluster using fsutil volume
    `$out = & fsutil volume filelayout `$vLetter /lcn:316279
    `$out | Select-Object -First 30
}
Dismount-DiskImage -ImagePath `$vhd -ErrorAction SilentlyContinue | Out-Null
Stop-Transcript
Set-Content -Path `$marker -Value "DONE" -Force
"@
Set-Content -Path $supScript -Value $supContent -Force

Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $supScript) -Verb RunAs -WindowStyle Hidden -PassThru | Out-Null

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 30000) {
    if (Test-Path $marker) { break }
    Start-Sleep -Milliseconds 500
}

if (Test-Path $logFile) {
    Get-Content $logFile
}
