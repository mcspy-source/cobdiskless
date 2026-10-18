# tools/diagnose_ibft_memory.ps1
# Boots VM1 to spinning dots and dumps the first 1MB of physical memory via QMP pmemsave

$WorkspaceRoot = (Resolve-Path "$PSScriptRoot\..").Path
$LogsDir = Join-Path $WorkspaceRoot "logs"
$RomFile = "$WorkspaceRoot\..\cobpxe\src\bin\8086100e.rom"
$absRom = (Resolve-Path $RomFile).Path.Replace('\', '/')

# Ensure daemon is running
$proc = Get-Process "cobdiskless_app" -ErrorAction SilentlyContinue
if (-not $proc) {
    Write-Host "[*] Starting cobdiskless_app..."
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$WorkspaceRoot\build\bin\cobdiskless_app.exe"
    $psi.Arguments = "--start-daemon --enable-trace-io"
    $psi.WorkingDirectory = "$WorkspaceRoot\build\bin"
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $null = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Seconds 3
}

# Remove stale wb
Remove-Item "E:\wb_525400123401.cow" -Force -ErrorAction SilentlyContinue

# Launch QEMU
Get-Process "qemu-system-x86_64" -ErrorAction SilentlyContinue | Stop-Process -Force
$qemuExe = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$qemuArgs = @(
    "-m", "4096",
    "-smp", "2",
    "-cpu", "max",
    "-accel", "tcg",
    "-display", "none",
    "-vga", "std",
    "-device", "e1000,netdev=net0,mac=52:54:00:12:34:01,romfile=$absRom",
    "-netdev", "tap,id=net0,ifname=OpenVPN TAP-Windows6",
    "-boot", "order=n",
    "-qmp", "tcp:127.0.0.1:4444,server,nowait",
    "-object", "filter-dump,id=f1,netdev=net0,file=$($LogsDir.Replace('\','/'))/vm1.pcap",
    "-serial", "none"
)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $qemuExe
$psi.Arguments = [string]::Join(" ", ($qemuArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }))
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$qemuProc = [System.Diagnostics.Process]::Start($psi)

Write-Host "[+] QEMU launched (PID: $($qemuProc.Id)), connecting QMP..."
Start-Sleep -Seconds 2

$tcpClient = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 4444)
$stream = $tcpClient.GetStream()
$reader = New-Object System.IO.StreamReader($stream)
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
$null = $reader.ReadLine() # greeting
$writer.WriteLine('{"execute": "qmp_capabilities"}')
$null = $reader.ReadLine()
Write-Host "[+] QMP connected. Waiting 25 seconds for Windows winload/kernel takeover..."

Start-Sleep -Seconds 90

$dumpPath = "$LogsDir\low_mem.bin".Replace('\', '/')
Write-Host "[*] Issuing pmemsave for 0x0 to 0x100000 (1MB) -> $dumpPath..."
$writer.WriteLine('{"execute": "pmemsave", "arguments": {"val": 0, "size": 1048576, "filename": "' + $dumpPath + '"}}')
$res = $reader.ReadLine()
Write-Host "[+] pmemsave result: $res"

Start-Sleep -Seconds 2

$writer.Close()
$reader.Close()
$stream.Close()
$tcpClient.Close()
Stop-Process -Id $qemuProc.Id -Force

if (Test-Path "$LogsDir\low_mem.bin") {
    $bytes = [System.IO.File]::ReadAllBytes("$LogsDir\low_mem.bin")
    Write-Host "[+] Dump size: $($bytes.Length) bytes"
    
    # Search for 'iBFT' (0x69 0x42 0x46 0x54) and 'IBFT' (0x49 0x42 0x46 0x54)
    $found = @()
    for ($i = 0; $i -lt $bytes.Length - 4; $i++) {
        if ($bytes[$i] -eq 0x69 -and $bytes[$i+1] -eq 0x42 -and $bytes[$i+2] -eq 0x46 -and $bytes[$i+3] -eq 0x54) {
            $found += $i
        }
        if ($bytes[$i] -eq 0x49 -and $bytes[$i+1] -eq 0x42 -and $bytes[$i+2] -eq 0x46 -and $bytes[$i+3] -eq 0x54) {
            $found += $i
        }
    }
    
    Write-Host "[*] Total iBFT occurrences found: $($found.Count)"
    foreach ($offset in $found) {
        $hex = "0x" + $offset.ToString("X5")
        Write-Host "    Found signature at offset: $hex ($offset)"
        
        # Read header
        $sig = [System.Text.Encoding]::ASCII.GetString($bytes, $offset, 4)
        $len = [BitConverter]::ToUInt32($bytes, $offset + 4)
        $oemId = [System.Text.Encoding]::ASCII.GetString($bytes, $offset + 10, 6)
        $oemTableId = [System.Text.Encoding]::ASCII.GetString($bytes, $offset + 16, 8)
        Write-Host "    Sig: $sig, Length: $len, OEM ID: $oemId, OEM Table: $oemTableId"
        
        # Dump printable strings in the table
        $tableBytes = New-Object byte[] $len
        [Array]::Copy($bytes, $offset, $tableBytes, 0, [Math]::Min($len, $bytes.Length - $offset))
        $tableStr = [System.Text.Encoding]::ASCII.GetString($tableBytes)
        [regex]::Matches($tableStr, '[A-Za-z0-9.:\-_]{4,60}') | ForEach-Object {
            Write-Host "      Table String: $($_.Value)"
        }
    }

    # Search for RSDP
    Write-Host "[*] Searching for RSDP..."
    for ($i = 0; $i -lt $bytes.Length - 8; $i += 16) {
        $sig = [System.Text.Encoding]::ASCII.GetString($bytes, $i, 8)
        if ($sig -eq "RSD PTR ") {
            $rsdtAddr = [BitConverter]::ToUInt32($bytes, $i + 16)
            Write-Host "    Found RSDP at 0x$($i.ToString('X5')), RSDT Address: 0x$($rsdtAddr.ToString('X8'))"
            if ($rsdtAddr -lt $bytes.Length) {
                $rsdtLen = [BitConverter]::ToUInt32($bytes, $rsdtAddr + 4)
                $numEntries = [int](($rsdtLen - 36) / 4)
                Write-Host "    RSDT Length: $rsdtLen, Entry Count: $numEntries"
                for ($e = 0; $e -lt $numEntries; $e++) {
                    $entryPtr = [BitConverter]::ToUInt32($bytes, $rsdtAddr + 36 + ($e * 4))
                    if ($entryPtr -lt $bytes.Length) {
                        $tblSig = [System.Text.Encoding]::ASCII.GetString($bytes, $entryPtr, 4)
                        Write-Host "      Table $e : $tblSig at 0x$($entryPtr.ToString('X8'))"
                    }
                }
            }
        }
    }
}
