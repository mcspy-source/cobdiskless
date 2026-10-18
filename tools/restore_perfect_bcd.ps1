$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\restore_perfect_bcd.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\restore_perfect_bcd.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\restore_bcd_run.ps1"
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

`$bootLetter = `$null
foreach (`$p in `$parts) {
    if (`$p.DriveLetter) {
        `$letter = "`$(`$p.DriveLetter):"
        if (Test-Path "`$letter\Boot\BCD") { `$bootLetter = `$letter; break }
    }
}

if (-not `$bootLetter) {
    # Assign drive letter if missing
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
    `$bcdPath = "`$bootLetter\Boot\BCD"
    Write-Host "[3/6] Restoring Original Type 6 Locate Device Elements in BCD..."
    
    & reg.exe load HKLM\RESTORE_BCD "`$bcdPath"
    Start-Sleep -Milliseconds 500
    
    # Exact binary blob for Type 6 (Locate Device) pointing to Partition 2 (offset 0x22600000, disk signature 0x3E128489)
    [byte[]]`$part2Blob = 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x48,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x00,0x00,0x60,0x22,0x00,0x00,0x00,0x00, `
                          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x01,0x00,0x00,0x00, `
                          0x89,0x84,0x12,0x3E, `
                          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00

    # Exact binary blob for Type 6 (Locate Device) pointing to Partition 1 (offset 0x00100000, disk signature 0x3E128489)
    [byte[]]`$part1Blob = 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x48,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00, `
                          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, `
                          0x01,0x00,0x00,0x00, `
                          0x89,0x84,0x12,0x3E, `
                          0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00

    # Target the Windows Boot Loader object
    `$loaderGuids = @("{8ef8a6b1-b875-11f1-826b-c774f3307f8b}")
    # Also check if there is a {default} alias
    `$allObjs = Get-ChildItem "HKLM:\RESTORE_BCD\Objects" -Name
    foreach (`$g in `$allObjs) {
        if (Test-Path "HKLM:\RESTORE_BCD\Objects\`$g\Elements\12000002") {
            `$pathVal = (Get-ItemProperty "HKLM:\RESTORE_BCD\Objects\`$g\Elements\12000002").Element
            if (`$pathVal -like "*winload.exe*") {
                Write-Host "  Injecting Partition 2 Locate blob into Loader `$g..."
                Set-ItemProperty -Path "HKLM:\RESTORE_BCD\Objects\`$g\Elements\11000001" -Name "Element" -Value `$part2Blob -Type Binary -Force
                Set-ItemProperty -Path "HKLM:\RESTORE_BCD\Objects\`$g\Elements\21000001" -Name "Element" -Value `$part2Blob -Type Binary -Force
            }
        }
        if (Test-Path "HKLM:\RESTORE_BCD\Objects\`$g\Elements\24000001") {
            # This is {bootmgr}
            Write-Host "  Injecting Partition 1 Locate blob into Bootmgr `$g..."
            Set-ItemProperty -Path "HKLM:\RESTORE_BCD\Objects\`$g\Elements\11000001" -Name "Element" -Value `$part1Blob -Type Binary -Force
        }
    }

    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg.exe unload HKLM\RESTORE_BCD
    Write-Host "  Successfully restored BCD binary elements."
} else {
    Write-Error "Failed to locate BCD!"
}

Write-Host '[4/6] Dismounting VHD and locking Read-Only...'
Dismount-DiskImage -ImagePath 'G:\G\csmw10.VHD' -ErrorAction SilentlyContinue | Out-Null
Start-Sleep -Seconds 2
attrib +r 'G:\G\csmw10.VHD'

Write-Host '[5/6] Purging writeback overlay files...'
Remove-Item -Path 'E:\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'E:\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue
Remove-Item -Path 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\writeback\wb_6c4b902f61eb.cow' -Force -ErrorAction SilentlyContinue

Write-Host '[6/6] Launching newly built cobdiskless_app...'
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

Write-Host "Triggering BCD Type 6 Locate injection..."
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
