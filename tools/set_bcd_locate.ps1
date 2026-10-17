$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\set_bcd_locate.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\set_bcd_locate.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\set_locate_run.ps1"
$supContent = @"
Start-Transcript -Path '$logFile' -Force

Write-Host '[1/5] Stopping cobdiskless_app...'
Stop-Process -Name 'cobdiskless_app' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host '[2/5] Unlocking and Mounting VHD...'
attrib -r 'G:\G\csmw10.VHD'
Mount-DiskImage -ImagePath 'G:\G\csmw10.VHD' -Access ReadWrite | Out-Null
Start-Sleep -Seconds 2

`$d = Get-DiskImage -ImagePath 'G:\G\csmw10.VHD' | Get-Disk
`$parts = Get-Partition -DiskNumber `$d.Number | Where-Object { `$_.Type -ne 'Reserved' }

`$bootLetter = `$null
foreach (`$p in `$parts) {
    if (`$p.DriveLetter) {
        `$letter = "`$(`$p.DriveLetter):"
        if (Test-Path "`$letter\Boot\BCD") { `$bootLetter = `$letter; break }
    }
}

if (-not `$bootLetter) {
    for (`$c = [char]'Y'; `$c -ge [char]'K'; `$c--) {
        `$cand = "`$([char]`$c):"
        if (-not (Get-PSDrive -Name ([char]`$c) -ErrorAction SilentlyContinue)) {
            Set-Partition -DiskNumber `$d.Number -PartitionNumber 1 -NewDriveLetter ([char]`$c) -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 500
            if (Test-Path "`$cand\Boot\BCD") { `$bootLetter = `$cand; break }
        }
    }
}

Write-Host "  Boot Partition: `$bootLetter"

if (`$bootLetter -and (Test-Path "`$bootLetter\Boot\BCD")) {
    `$bcd = "`$bootLetter\Boot\BCD"
    Write-Host "[3/5] Setting {bootmgr} to boot and {default} to locate in `$bcd..."
    
    # 1. bootmgr device -> boot
    & bcdedit.exe /store `$bcd /set "{bootmgr}" device boot
    
    # 2. default loader device -> locate
    & bcdedit.exe /store `$bcd /set "{default}" device locate
    
    # 3. default loader osdevice -> locate
    & bcdedit.exe /store `$bcd /set "{default}" osdevice locate
    
    # 4. default loader path
    & bcdedit.exe /store `$bcd /set "{default}" path "\Windows\system32\winload.exe"
    
    # 5. systemroot
    & bcdedit.exe /store `$bcd /set "{default}" systemroot "\Windows"
    
    Write-Host "  Updated BCD output:"
    & bcdedit.exe /store `$bcd /enum all
} else {
    Write-Error "Failed to locate BCD!"
}

Write-Host '[4/5] Dismounting VHD and locking Read-Only...'
Dismount-DiskImage -ImagePath 'G:\G\csmw10.VHD' -ErrorAction SilentlyContinue | Out-Null
Start-Sleep -Seconds 2
attrib +r 'G:\G\csmw10.VHD'

Write-Host '[5/5] Purging writeback overlay files & restarting daemon...'
Remove-Item -Path 'E:\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'E:\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue

`$AppWorkDir = 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin'
`$AppExe = "`$AppWorkDir\cobdiskless_app.exe"
Set-Location `$AppWorkDir
`$p = Start-Process -FilePath `$AppExe -ArgumentList '--start-daemon --enable-trace-io' -WorkingDirectory `$AppWorkDir -PassThru

for (`$i = 0; `$i -lt 30; `$i++) {
    `$u67 = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { `$_.LocalPort -eq 67 }).Count -gt 0
    `$t18 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { `$_.State -eq 'Listen' -and `$_.LocalPort -eq 18080 }).Count -gt 0
    `$t32 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { `$_.State -eq 'Listen' -and `$_.LocalPort -eq 3260 }).Count -gt 0
    if (`$u67 -and `$t18 -and `$t32) {
        Write-Host "  cobdiskless_app ready (PID `$(`$p.Id))."
        break
    }
    Start-Sleep -Milliseconds 500
}

Stop-Transcript
Set-Content -Path '$marker' -Value "SUCCESS" -Force

while (`$true) {
    if (`$p.HasExited) { break }
    Start-Sleep -Seconds 2
}
"@

Set-Content -Path $supScript -Value $supContent -Force

Write-Host "Triggering BCD LOCATE configuration..."
Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $supScript) -Verb RunAs -WindowStyle Hidden

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 35000) {
    if (Test-Path $marker) {
        Write-Host "[+] Finished with status: $(Get-Content $marker -Raw)" -ForegroundColor Green
        break
    }
    Start-Sleep -Milliseconds 500
}

if (Test-Path $logFile) {
    Get-Content $logFile
}
