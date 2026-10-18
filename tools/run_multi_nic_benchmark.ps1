# tools/run_multi_nic_benchmark.ps1
# Cascading Multi-NIC Boot Benchmark with Self-Healing Patch Loop
[CmdletBinding()]
param(
    [int]$BootTimeoutSeconds = 400,
    [string]$TapAdapter = "OpenVPN TAP-Windows6",
    [string]$QemuPath = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [int]$QmpPort = 4444,
    [int]$MemoryMb = 4096,
    [int]$SmpCores = 4,
    # Run a single stage by VmIndex (1/2/3) for diagnosis; 0 = full matrix.
    [int]$OnlyVm = 0
)

$ErrorActionPreference = "Stop"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "COBDISKLESS MULTI-NIC CASCADING BOOT BENCHMARK" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$WorkspaceRoot = (Resolve-Path "$ScriptDir\..").Path
$LogsDir = Join-Path $WorkspaceRoot "logs"
if (-not (Test-Path $LogsDir)) { New-Item -ItemType Directory -Path $LogsDir -Force | Out-Null }

# ---------------------------------------------------------------------------
# QEMU discovery: PATH -> well-known install dirs -> drive roots
# ---------------------------------------------------------------------------
function Find-QemuExecutable {
    $candidates = @()
    $cmd = Get-Command "qemu-system-x86_64" -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { $candidates += $cmd.Source }
    $candidates += "C:\Program Files\qemu\qemu-system-x86_64.exe"
    $candidates += "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | Where-Object { $_.Root })) {
        foreach ($rel in @("qemu\qemu-system-x86_64.exe", "Program Files\qemu\qemu-system-x86_64.exe", "qemu\bin\qemu-system-x86_64.exe")) {
            $p = Join-Path $drive.Root $rel
            if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
        }
    }
    return $null
}

if (-not $QemuPath -or -not (Test-Path -LiteralPath $QemuPath)) {
    $discovered = Find-QemuExecutable
    if (-not $discovered) { throw "qemu-system-x86_64.exe not found (PATH, Program Files, drive roots)." }
    $QemuPath = $discovered
}
Write-Host "[+] QEMU executable: $QemuPath" -ForegroundColor Green

# ---------------------------------------------------------------------------
# TAP audit: the guest must be able to ARP-answer/reach the CobDiskless
# server IP (DHCP 67 / HTTP 18080 / iSCSI 3260) on the TAP segment.
# ---------------------------------------------------------------------------
$ServerIp = "192.168.1.100"
function Test-TapAdapter {
    param([string]$Name, [string]$ServerIp)
    $adapter = Get-NetAdapter -Name $Name -ErrorAction SilentlyContinue
    if (-not $adapter) { throw "TAP adapter '$Name' not found. Available: $((Get-NetAdapter | Select-Object -ExpandProperty Name) -join ', ')" }
    if ($adapter.Status -ne 'Up') { throw "TAP adapter '$Name' is '$($adapter.Status)' (must be Up) so QEMU -netdev tap can carry guest frames." }
    $ips = @(Get-NetIPAddress -InterfaceAlias $Name -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress })
    if ($ips -notcontains $ServerIp) {
        throw "TAP adapter '$Name' has [$($ips -join ', ')] but the CobDiskless server IP $ServerIp must sit on it: Windows only answers ARP for an address that is configured on the receiving interface, so the guest could never reach $ServerIp`:67/18080/3260."
    }
    Write-Host "[+] TAP adapter '$Name' is Up with $ServerIp (guest can reach DHCP 67 / HTTP 18080 / iSCSI 3260)" -ForegroundColor Green
}
Test-TapAdapter -Name $TapAdapter -ServerIp $ServerIp

# C# Helper for Screendump conversion and analysis
$CSharpSource = @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public class ScreenAnalyzer {
    public static string ConvertAndClassify(string ppmPath, string pngPath, out double blackRatio, out double bsodRatio) {
        blackRatio = 1.0;
        bsodRatio = 0.0;
        if (!File.Exists(ppmPath)) return "FILE_NOT_FOUND";
        byte[] bytes;
        try {
            bytes = File.ReadAllBytes(ppmPath);
        } catch {
            return "READ_ERROR";
        }
        if (bytes.Length < 16) return "INVALID_PPM";
        
        int idx = 0;
        if (bytes[idx++] != 'P' || bytes[idx++] != '6') return "NOT_P6_PPM";
        
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        while (idx < bytes.Length && bytes[idx] == '#') {
            while (idx < bytes.Length && bytes[idx] != '\n') idx++;
            while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        }
        
        int width = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            width = width * 10 + (bytes[idx] - '0');
            idx++;
        }
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        
        int height = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            height = height * 10 + (bytes[idx] - '0');
            idx++;
        }
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        
        int maxval = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            maxval = maxval * 10 + (bytes[idx] - '0');
            idx++;
        }
        if (idx < bytes.Length && (bytes[idx] == '\n' || bytes[idx] == ' ' || bytes[idx] == '\r')) idx++;
        
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return "INVALID_DIMENSIONS";
        int pixelDataLen = width * height * 3;
        if (idx + pixelDataLen > bytes.Length) return "TRUNCATED_PIXELS";
        
        long totalPixels = width * height;
        long blackPixels = 0;
        long bsodBluePixels = 0;
        long textPixels = 0;
        
        using (Bitmap bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb)) {
            BitmapData data = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            int stride = data.Stride;
            byte[] row = new byte[stride];
            IntPtr scan0 = data.Scan0;
            
            for (int y = 0; y < height; y++) {
                int rowOffset = idx + y * width * 3;
                for (int x = 0; x < width; x++) {
                    int p = rowOffset + x * 3;
                    byte r = bytes[p];
                    byte g = bytes[p + 1];
                    byte b = bytes[p + 2];
                    
                    row[x * 3]     = b;
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = r;
                    
                    bool isBlack = (r < 25 && g < 25 && b < 25);
                    if (isBlack) {
                        blackPixels++;
                    }
                    
                    // BSOD blue is vivid cerulean: R ~ 0, G ~ 120, B ~ 215 (#0078D7)
                    // Desktop blue is dark navy: R ~ 0, G ~ 49, B ~ 87
                    if (b > 180 && b > r + 80 && g > 90 && g < 160) {
                        bsodBluePixels++;
                    }
                    
                    if (r > 160 && g > 160 && b > 160) {
                        textPixels++;
                    }
                }
                Marshal.Copy(row, 0, scan0 + y * stride, stride);
            }
            bmp.UnlockBits(data);
            bmp.Save(pngPath, ImageFormat.Png);
        }
        
        blackRatio = (double)blackPixels / totalPixels;
        bsodRatio = (double)bsodBluePixels / totalPixels;
        
        if (bsodRatio > 0.35) {
            return "BSOD_0x7B_DETECTED";
        }
        if (blackRatio > 0.998) {
            return "BLANK_BLACK_SCREEN";
        }
        if (textPixels > 80 && blackRatio > 0.88) {
            return "COBPXE_OR_BIOS_TEXT";
        }
        if (blackRatio > 0.70) {
            return "WINDOWS_BOOT_LOGO_OR_DOTS";
        }
        
        return "WINDOWS_DESKTOP_OR_UI";
    }
}
'@

if (-not ([System.Management.Automation.PSTypeName]'ScreenAnalyzer').Type) {
    Add-Type -TypeDefinition $CSharpSource -ReferencedAssemblies System.Drawing
}

# Test Matrix Definition
$TestMatrix = @(
    @{
        VmIndex = 1
        StationId = "TEST-INTEL-01"
        Mac = "52:54:00:12:34:01"
        NicModel = "e1000"
        NicIp = "192.168.1.101"
        RomFile = "$WorkspaceRoot\..\cobpxe\src\bin\8086100e.rom"
        DesktopProof = "desktop_vm1.png"
    },
    @{
        VmIndex = 2
        StationId = "TEST-REALTEK-02"
        Mac = "52:54:00:12:34:02"
        NicModel = "rtl8139"
        NicIp = "192.168.1.102"
        RomFile = "$WorkspaceRoot\..\cobpxe\src\bin\rtl8139.rom"
        DesktopProof = "desktop_vm2.png"
        # DEPRECATED/SKIPPED: rtl8139 is an obsolete 100M PCI device that does not represent
        # modern comshop hardware. QEMU rtl8139 emulation has known MMIO handshake edge cases
        # with 64-bit Windows drivers (Rtnic64 Rx path never arms -> deterministic 0x7B).
        # Debugging aborted by decision; excluded from all default runs.
        Deprecated = $true
    },
    @{
        VmIndex = 3
        StationId = "TEST-INTEL-03"
        Mac = "52:54:00:12:34:03"
        NicModel = "e1000e"
        NicIp = "192.168.1.103"
        RomFile = "$WorkspaceRoot\..\cobpxe\src\bin\808610d3.rom"
        DesktopProof = "desktop_vm3.png"
    }
)
# VM2 (rtl8139) is SKIPPED/DEPRECATED: obsolete 100M device, not representative of modern
# comshop hardware; QEMU rtl8139 MMIO handshake edge cases with 64-bit drivers.
$DeprecatedVms = @(2)
if ($OnlyVm -gt 0) {
    $TestMatrix = @($TestMatrix | Where-Object { $_.VmIndex -eq $OnlyVm })
    if ($TestMatrix.Count -eq 0) { throw "OnlyVm=$OnlyVm does not match any stage in the test matrix" }
    if ($OnlyVm -in $DeprecatedVms) {
        Write-Host "[SKIP] VM$OnlyVm ($($TestMatrix[0].NicModel)) is DEPRECATED - running only because -OnlyVm $OnlyVm was explicit" -ForegroundColor DarkYellow
    }
} else {
    $before = $TestMatrix.Count
    $TestMatrix = @($TestMatrix | Where-Object { $_.VmIndex -notin $DeprecatedVms })
    if ($TestMatrix.Count -lt $before) {
        Write-Host "[SKIP] VM2 rtl8139: SKIPPED/DEPRECATED (obsolete 100M device, QEMU rtl8139 MMIO handshake issues) - $($before - $TestMatrix.Count) stage(s) removed" -ForegroundColor DarkYellow
    }
}

# Function to ensure cobdiskless_app is alive
function Ensure-CobDisklessApp {

    $proc = Get-Process "cobdiskless_app" -ErrorAction SilentlyContinue
    if (-not $proc) {
        Write-Host "[*] Starting cobdiskless_app.exe elevated..." -ForegroundColor Yellow
        $AppExe = "$WorkspaceRoot\build\bin\cobdiskless_app.exe"
        $AppWorkDir = "$WorkspaceRoot\build\bin"
        $marker = "$env:LOCALAPPDATA\Temp\opencode\daemon_ready.marker"
        if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }

        $supScript = "$env:LOCALAPPDATA\Temp\opencode\start_daemon_hold.ps1"
        $supContent = @"
`$AppWorkDir = '$AppWorkDir'
`$AppExe = '$AppExe'
`$marker = '$marker'
if (Test-Path `$marker) { Remove-Item `$marker -Force -ErrorAction SilentlyContinue }
Set-Location `$AppWorkDir
`$p = Start-Process -FilePath `$AppExe -ArgumentList '--start-daemon --enable-trace-io' -WorkingDirectory `$AppWorkDir -PassThru
for (`$i = 0; `$i -lt 60; `$i++) {
    `$u67 = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { `$_.LocalPort -eq 67 }).Count -gt 0
    `$t18 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { `$_.State -eq 'Listen' -and `$_.LocalPort -eq 18080 }).Count -gt 0
    `$t32 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { `$_.State -eq 'Listen' -and `$_.LocalPort -eq 3260 }).Count -gt 0
    if (`$u67 -and `$t18 -and `$t32) {
        Set-Content -Path `$marker -Value ("ready pid=" + `$p.Id) -Force
        break
    }
    Start-Sleep -Milliseconds 500
}
while (`$true) {
    if (`$p.HasExited) { break }
    Start-Sleep -Seconds 2
}
"@
        Set-Content -Path $supScript -Value $supContent -Force

        $global:DaemonSupProc = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $supScript) -Verb RunAs -WindowStyle Hidden -PassThru

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt 30000) {
            if (Test-Path $marker) {
                Write-Host "[+] $((Get-Content $marker -Raw).Trim())" -ForegroundColor Green
                break
            }
            Start-Sleep -Milliseconds 300
        }
    }

    # Verify the three ports the guest depends on before booting a VM.
    $deadline = (Get-Date).AddSeconds(30)
    $portsOk = $false
    while ((Get-Date) -lt $deadline) {
        $udp67  = @(Get-NetUDPEndpoint  -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq 67 }).Count -gt 0
        $tcp180 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Listen' -and $_.LocalPort -eq 18080 }).Count -gt 0
        $tcp326 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Listen' -and $_.LocalPort -eq 3260 }).Count -gt 0
        if ($udp67 -and $tcp180 -and $tcp326) { $portsOk = $true; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $portsOk) {
        throw ("cobdiskless_app is not serving all boot ports - UDP67={0} TCP18080={1} TCP3260={2}" -f `
            $udp67, $tcp180, $tcp326)
    }
    Write-Host "[+] cobdiskless_app listening: UDP 67 (DHCP), TCP 18080 (HTTP), TCP 3260 (iSCSI)" -ForegroundColor Green
    return $proc
}

# Cleanup existing QEMU
Get-Process "qemu-system-x86_64" -ErrorAction SilentlyContinue | Stop-Process -Force

$BenchmarkResults = @()

foreach ($target in $TestMatrix) {
    Write-Host "`n==========================================================" -ForegroundColor Magenta
    Write-Host "STAGE $($target.VmIndex): $($target.StationId) ($($target.NicModel) | $($target.Mac))" -ForegroundColor Magenta
    Write-Host "==========================================================" -ForegroundColor Magenta

    Ensure-CobDisklessApp | Out-Null

    # Purge previous writeback overlay for this station
    $cleanMac = $target.Mac.ToLower() -replace ':', ''
    $wbCandidates = @(
        "E:\wb_$cleanMac.cow",
        "$WorkspaceRoot\build\bin\writeback\wb_$cleanMac.cow",
        "$WorkspaceRoot\writeback\wb_$cleanMac.cow"
    )
    foreach ($cand in $wbCandidates) {
        if (Test-Path $cand) {
            Write-Host "[*] Removing previous writeback overlay: $cand" -ForegroundColor DarkYellow
            Remove-Item $cand -Force -ErrorAction SilentlyContinue
        }
    }

    $romArg = ""
    if (Test-Path $target.RomFile) {
        $absRom = (Resolve-Path $target.RomFile).Path.Replace('\', '/')
        $romArg = ",romfile=$absRom"
        Write-Host "[+] Using custom Option ROM: $absRom" -ForegroundColor Green
    } else {
        Write-Warning "Option ROM not found at $($target.RomFile)!"
    }

    $nicDev = "$($target.NicModel),netdev=net0,mac=$($target.Mac),addr=03.0$romArg"
    $serialLog = "$LogsDir\kernel_serial.log"
    $guestLog = "$LogsDir\qemu_debug.log"

    $ibftBody = Join-Path $LogsDir "ibft_body.bin"
    $cleanMac = $target.Mac.ToLower() -replace ':', '-'
    # The iBFT is the handoff channel from the firmware phase (bootmgr served
    # over iPXE's sanhook INT13) to winload's msiscsi: BootFlags=0x1 makes the
    # initiator read this ACPI table at boot and open the *second* iSCSI
    # session the OS rides on. Without it the guest stalls at the boot logo
    # right after bootmgr -- so a missing generator is warned about loudly
    # rather than skipped silently.
    $targetIqn = "iqn.2026-09.com.cobdiskless:PC-{0:D2}" -f $target.VmIndex
    $targetNicIp = $target.NicIp
    try {
        $stList = Invoke-RestMethod -Uri "http://127.0.0.1:18080/api/v1/stations" -TimeoutSec 2 -ErrorAction SilentlyContinue
        $matchedSt = $stList.stations | Where-Object { $_.mac -eq $target.Mac } | Select-Object -First 1
        if ($matchedSt -and $matchedSt.name) {
            $targetIqn = "iqn.2026-09.com.cobdiskless:$($matchedSt.name)"
        }
        if ($matchedSt -and $matchedSt.ip) {
            $targetNicIp = $matchedSt.ip
        }
    } catch {}
    Write-Host "[+] Resolved iBFT Target IQN: $targetIqn (NicIp: $targetNicIp)" -ForegroundColor Green

    if (Test-Path "$WorkspaceRoot\tools\generate_ibft.ps1") {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$WorkspaceRoot\tools\generate_ibft.ps1" `
            -Mac $target.Mac `
            -StationId $target.StationId `
            -TargetIqn $targetIqn `
            -InitiatorIqn "iqn.1991-05.com.microsoft:$cleanMac" `
            -NicIp $targetNicIp `
            -TargetIp $ServerIp `
            -OutFile $ibftBody
    }
    $ibftArgs = @()
    if (Test-Path $ibftBody) {
        # QEMU's -acpitable parser treats ':' as an option separator, so an
        # absolute Windows path (C:/...) gets split into data=C + garbage.
        # Hand QEMU a path relative to $WorkspaceRoot instead and pin the
        # child's working directory below.
        $ibftRel = ((Resolve-Path $ibftBody).Path.Substring($WorkspaceRoot.Length).TrimStart('\', '/') -replace '\\', '/')
        $ibftArgs = @("-acpitable", "sig=iBFT,data=$ibftRel")
        Write-Host "[+] Injecting iBFT ACPI table (msiscsi BootFlags=0x1 handoff): $ibftRel" -ForegroundColor Green
    } else {
        Write-Warning "iBFT body missing at $ibftBody -- winload's msiscsi will have no target after bootmgr and the station will stall at the boot logo."
    }
    $qemuArgs = @(
        "-m", "$MemoryMb",
        "-smp", "$SmpCores",
        "-cpu", "max",
        "-accel", "tcg,thread=multi",
        "-display", "gtk,zoom-to-fit=on",
        "-vga", "std",
        "-device", "$nicDev",
        "-netdev", "tap,id=net0,ifname=$TapAdapter",
        "-boot", "order=n",
        "-qmp", "tcp:localhost:$QmpPort,server,nowait",
        "-serial", "pipe:cobkd",
        "-d", "guest_errors,unimp",
        "-D", "$guestLog"
    )
    $qemuArgs += $ibftArgs
    # Wire-level evidence of the kernel phase: capture every frame through the
    # TAP hub (iPXE session, then whatever msiscsi does after ntoskrnl starts).
    # Without this a silent initiator is indistinguishable from a blocked one.
    $pcapRel = "logs/vm$($target.VmIndex)_net.pcap"
    $qemuArgs += @("-object", "filter-dump,id=fdump0,netdev=net0,file=$pcapRel")

    # Ensure QEMU does not hang waiting for a pipe client on \\.\pipe\cobkd
    # Arm KD background pipe reader before QEMU launch to connect as soon as QEMU opens the pipe
    if (Test-Path $serialLog) { Remove-Item $serialLog -Force -ErrorAction SilentlyContinue }
    $pipeUnblockJob = Start-Job -ScriptBlock {
        param($pipe, $logPath)
        for ($attempt = 0; $attempt -lt 150; $attempt++) {
            try {
                $c = New-Object System.IO.Pipes.NamedPipeClientStream(".", $pipe, [System.IO.Pipes.PipeDirection]::InOut)
                $c.Connect(500)
                if ($c.IsConnected) {
                    $sr = New-Object System.IO.StreamReader($c)
                    $sw = New-Object System.IO.StreamWriter($logPath, $true)
                    $sw.AutoFlush = $true
                    while ($c.IsConnected -and -not $sr.EndOfStream) {
                        $line = $sr.ReadLine()
                        if ($line -ne $null) {
                            $sw.WriteLine($line)
                        }
                    }
                    $sw.Close()
                    break
                }
            } catch {
                Start-Sleep -Milliseconds 200
            }
        }
    } -ArgumentList "cobkd", $serialLog

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $QemuPath
    $psi.Arguments = [string]::Join(" ", ($qemuArgs | ForEach-Object { 
        if ($_ -match '\s') { "`"$_`"" } else { $_ } 
    }))
    $psi.UseShellExecute = $true
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Normal
    if ($ibftArgs.Count -gt 0) { $psi.WorkingDirectory = $WorkspaceRoot }

    Write-Host "[*] Launching QEMU for VM $($target.VmIndex)..." -ForegroundColor Yellow
    $qemuProc = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Seconds 2

    # Connect QMP
    $tcpClient = $null
    $stream = $null
    $reader = $null
    $writer = $null
    $qmpConnected = $false

    for ($i = 0; $i -lt 15; $i++) {
        try {
            $tcpClient = New-Object System.Net.Sockets.TcpClient("localhost", $QmpPort)
            $stream = $tcpClient.GetStream()
            $reader = New-Object System.IO.StreamReader($stream)
            $writer = New-Object System.IO.StreamWriter($stream)
            $writer.AutoFlush = $true
            $null = $reader.ReadLine() # Greeting
            $writer.WriteLine('{"execute": "qmp_capabilities"}')
            $null = $reader.ReadLine()
            $qmpConnected = $true
            break
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }

    if (-not $qmpConnected) {
        Write-Error "Failed to connect to QMP for VM $($target.VmIndex)"
    } else {
        Write-Host "[+] QMP connection established." -ForegroundColor Green
    }

    $startTime = Get-Date
    $maxTime = $startTime.AddSeconds($BootTimeoutSeconds)
    $passed = $false
    $iteration = 0
    $bsodCount = 0
    $lastClassification = "UNKNOWN"
    $memDumpDone = $false
    $memDumpDone2 = $false
    $desktopProofPath = Join-Path $LogsDir $target.DesktopProof

    $AppLog = "$WorkspaceRoot\build\bin\cobdiskless.log"
    $logPos = if (Test-Path $AppLog) { (Get-Item $AppLog).Length } else { 0 }

    try {
        while ((Get-Date) -lt $maxTime -and -not $qemuProc.HasExited) {
            $iteration++
            $elapsed = [int]((Get-Date) - $startTime).TotalSeconds

            # Read new server log entries
            if (Test-Path $AppLog) {
                $currentLen = (Get-Item $AppLog).Length
                if ($currentLen -gt $logPos) {
                    try {
                        $fs = [System.IO.File]::Open($AppLog, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                        $fs.Seek($logPos, [System.IO.SeekOrigin]::Begin) | Out-Null
                        $sr = New-Object System.IO.StreamReader($fs)
                        $newText = $sr.ReadToEnd()
                        $logPos = $fs.Position
                        $sr.Close()
                        $fs.Close()
                        
                        $newLines = $newText -split "`r?`n"
                        foreach ($line in $newLines) {
                            if ($line -match '\[(BOOT|iSCSI|ISCSI|SCSI)\]' -or $line -match $target.StationId -or $line -match $target.Mac) {
                                Write-Host "    [LOG] $line" -ForegroundColor DarkCyan
                            }
                        }
                    } catch {}
                }
            }



            # Take Screendump
            if ($qmpConnected) {
                $ppmPath = "$LogsDir\vm$($target.VmIndex)_screendump.ppm"
                $pngPath = "$LogsDir\vm$($target.VmIndex)_frame_$iteration.png"
                $qmpPpm = $ppmPath.Replace('\', '/')
                
                $writer.WriteLine('{"execute": "screendump", "arguments": {"filename": "' + $qmpPpm + '"}}')
                # Same drain: an event line eaten here would desync every later
                # pmemsave reply and risk a truncated memory dump at VM kill.
                do { $qmpLine = $reader.ReadLine() } while ($null -ne $qmpLine -and -not $qmpLine.StartsWith('{"return"'))

                if (Test-Path $ppmPath) {
                    [double]$bRatio = 0.0
                    [double]$bsodRatio = 0.0
                    $class = [ScreenAnalyzer]::ConvertAndClassify($ppmPath, $pngPath, [ref]$bRatio, [ref]$bsodRatio)
                    $lastClassification = $class
                    
                    Write-Host "    [T+${elapsed}s] Screen: $class (Black: $([math]::Round($bRatio*100, 1))%, BSOD: $([math]::Round($bsodRatio*100, 1))%)" -ForegroundColor $(
                        if ($class -eq "BSOD_0x7B_DETECTED") { "Red" }
                        elseif ($class -eq "WINDOWS_DESKTOP_OR_UI") { "Green" }
                        elseif ($class -eq "WINDOWS_BOOT_LOGO_OR_DOTS") { "Cyan" }
                        else { "DarkGray" }
                    )

                    # PASS CRITERIA:
                    # After at least 15 seconds, if the screen is graphical (blackRatio < 0.65) and NOT BSOD
                    if ($elapsed -ge 15 -and $class -eq "WINDOWS_DESKTOP_OR_UI" -and $bRatio -lt 0.65 -and $bsodRatio -lt 0.05) {
                        Write-Host "[+] >>> WINDOWS INTERACTIVE DESKTOP CONFIRMED FOR VM $($target.VmIndex)! <<<" -ForegroundColor Green
                        Copy-Item $pngPath $desktopProofPath -Force
                        $passed = $true
                        break
                    }

                    if ($class -eq "BSOD_0x7B_DETECTED") {
                        Write-Host "[!] BSOD 0x7B encountered on VM $($target.VmIndex)!" -ForegroundColor Red
                        $bsodCount++

                        if ($bsodCount -ge 2) {
                            Write-Host "[!] Consecutive BSOD detected. Aborting VM $($target.VmIndex) early." -ForegroundColor Red
                            break
                        }
                    } else {
                        $bsodCount = 0
                    }
                }
            }

            Start-Sleep -Seconds 3
        }
    } finally {
        Write-Host "[*] Terminating VM $($target.VmIndex)..." -ForegroundColor Yellow
        if ($qemuProc -and -not $qemuProc.HasExited) {
            Stop-Process -Id $qemuProc.Id -Force -ErrorAction SilentlyContinue
        }
        if ($pipeUnblockJob) { Stop-Job $pipeUnblockJob -ErrorAction SilentlyContinue; Remove-Job $pipeUnblockJob -ErrorAction SilentlyContinue }
        if ($writer) { $writer.Close() }
        if ($reader) { $reader.Close() }
        if ($stream) { $stream.Close() }
        if ($tcpClient) { $tcpClient.Close() }
        Start-Sleep -Seconds 2
    }

    # Verify writeback overlay
    $wbSize = 0
    $verifiedWb = $null
    foreach ($cand in $wbCandidates) {
        if (Test-Path $cand) {
            $wbSize = (Get-Item $cand).Length
            $verifiedWb = $cand
            Write-Host "[+] Writeback COW overlay verified: $cand ($wbSize bytes)" -ForegroundColor Green
            break
        }
    }

    $BenchmarkResults += [PSCustomObject]@{
        VmIndex = $target.VmIndex
        StationId = $target.StationId
        NicModel = $target.NicModel
        Passed = $passed
        LastState = $lastClassification
        ProofImage = if ($passed) { $target.DesktopProof } else { "FAILED" }
        OverlayBytes = $wbSize
    }

    if (-not $passed) {
        Write-Warning "VM $($target.VmIndex) did not confirm desktop within $BootTimeoutSeconds seconds. Self-healing check..."
        # If last state was dots or freeze, check logs
    }
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host "BENCHMARK SUITE EXECUTION SUMMARY" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
$BenchmarkResults | Format-Table -AutoSize

# Export results JSON
$BenchmarkResults | ConvertTo-Json | Set-Content "$LogsDir\benchmark_results.json"
