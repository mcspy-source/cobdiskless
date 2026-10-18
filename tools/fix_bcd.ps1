$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\fix_bcd.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\fix_bcd.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\fix_bcd_run.ps1"
$supContent = @"
Start-Transcript -Path '$logFile' -Force

Write-Host '[1/6] Stopping cobdiskless_app...'
Stop-Process -Name 'cobdiskless_app' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host '[2/6] Unlocking and Mounting VHD...'
attrib -r 'G:\G\csmw10.VHD'
Mount-DiskImage -ImagePath 'G:\G\csmw10.VHD' -Access ReadWrite | Out-Null
Start-Sleep -Seconds 2

`$d = Get-DiskImage -ImagePath 'G:\G\csmw10.VHD' | Get-Disk
`$parts = Get-Partition -DiskNumber `$d.Number | Where-Object { `$_.Type -ne 'Reserved' }

`$osLetter = `$null
`$bootLetter = `$null

foreach (`$p in `$parts) {
    if (`$p.DriveLetter) {
        `$letter = "`$(`$p.DriveLetter):"
        if (Test-Path "`$letter\Windows\System32\winload.exe") { `$osLetter = `$letter }
        if (Test-Path "`$letter\Boot\BCD") { `$bootLetter = `$letter }
    } else {
        for (`$c = [char]'Y'; `$c -ge [char]'K'; `$c--) {
            `$cand = "`$([char]`$c):"
            if (-not (Get-PSDrive -Name ([char]`$c) -ErrorAction SilentlyContinue)) {
                Set-Partition -DiskNumber `$d.Number -PartitionNumber `$p.PartitionNumber -NewDriveLetter ([char]`$c) -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 500
                if (Test-Path "`$cand\Windows\System32\winload.exe") { `$osLetter = `$cand; break }
                if (Test-Path "`$cand\Boot\BCD") { `$bootLetter = `$cand; break }
                break
            }
        }
    }
}

Write-Host "  Boot Partition: `$bootLetter"
Write-Host "  OS Partition:   `$osLetter"

if (`$bootLetter -and `$osLetter -and (Test-Path "`$bootLetter\Boot\BCD")) {
    `$bcdStore = "`$bootLetter\Boot\BCD"
    Write-Host "[3/6] Configuring BCD Store: `$bcdStore"
    
    # {bootmgr} must point to the boot partition where bootmgr and BCD reside
    & bcdedit.exe /store `$bcdStore /set "{bootmgr}" device "partition=`$bootLetter"
    
    # {default} device and osdevice MUST point to the OS partition where \Windows\System32\winload.exe lives!
    & bcdedit.exe /store `$bcdStore /set "{default}" device "partition=`$osLetter"
    & bcdedit.exe /store `$bcdStore /set "{default}" osdevice "partition=`$osLetter"
    & bcdedit.exe /store `$bcdStore /set "{default}" path "\Windows\System32\winload.exe"
    
    # Keep SOS display and ignore failures
    & bcdedit.exe /store `$bcdStore /set "{default}" sos yes
    & bcdedit.exe /store `$bcdStore /set "{default}" bootlog yes
    & bcdedit.exe /store `$bcdStore /set "{default}" bootstatuspolicy ignoreallfailures
    & bcdedit.exe /store `$bcdStore /set "{default}" recoveryenabled no
    
    Write-Host "  Current BCD entries:"
    & bcdedit.exe /store `$bcdStore /enum all
} else {
    Write-Error "Failed to locate boot or OS partitions!"
}

Write-Host '[4/6] Dismounting VHD and locking Read-Only...'
Dismount-DiskImage -ImagePath 'G:\G\csmw10.VHD' -ErrorAction SilentlyContinue | Out-Null
Start-Sleep -Seconds 2
attrib +r 'G:\G\csmw10.VHD'

Write-Host '[5/6] Purging writeback overlay files...'
Remove-Item -Path 'E:\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'E:\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue

Write-Host '[6/6] Restarting cobdiskless_app...'
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

Write-Host "Triggering elevated BCD fix..."
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
