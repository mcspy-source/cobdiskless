# tools/run_headless_qemu.ps1
# Automated Headless QEMU Boot & Live Storage Debugging Harness for CobDiskless
[CmdletBinding()]
param(
    [int]$DurationSeconds = 45,
    [string]$TapAdapter = "OpenVPN TAP-Windows6",
    [string]$Mac = "8c:89:a5:91:e8:e1",
    [string]$NicModel = "rtl8139",
    [string]$RomFile = "",
    [string]$QemuPath = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [int]$QmpPort = 4444,
    [int]$MemoryMb = 4096,
    [int]$SmpCores = 2,
    [switch]$KeepVmRunning
)

$ErrorActionPreference = "Stop"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "CobDiskless Headless QEMU Test Harness" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Locate workspace root
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$WorkspaceRoot = (Resolve-Path "$ScriptDir\..").Path
$LogsDir = Join-Path $WorkspaceRoot "logs"

if (-not (Test-Path $LogsDir)) {
    New-Item -ItemType Directory -Path $LogsDir -Force | Out-Null
}

# Ensure QEMU executable exists
if (-not (Test-Path $QemuPath)) {
    $found = Get-Command "qemu-system-x86_64.exe" -ErrorAction SilentlyContinue
    if ($found) {
        $QemuPath = $found.Source
    } else {
        throw "QEMU executable not found at '$QemuPath' or in PATH."
    }
}
Write-Host "[+] QEMU Path: $QemuPath" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Step 1: Purge Stale State
# ---------------------------------------------------------------------------
Write-Host "[*] Purging previous VM instances and stale test state..." -ForegroundColor Yellow

# Kill running QEMU processes
Get-Process "qemu-system-x86_64" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "    Killing stale QEMU process (PID: $($_.Id))..." -ForegroundColor DarkYellow
    Stop-Process -Id $_.Id -Force
}

# Remove stale log and screendump files
$StaleFiles = @(
    "$LogsDir\boot_screen*.ppm",
    "$LogsDir\boot_screen*.png",
    "$LogsDir\qemu_serial.log",
    "$LogsDir\qemu_guest.log"
)
foreach ($pattern in $StaleFiles) {
    Remove-Item $pattern -Force -ErrorAction SilentlyContinue
}

# Clean stale COW writeback files in common locations
$CowDirs = @(
    "$WorkspaceRoot\build\bin\writeback",
    "$WorkspaceRoot\writeback",
    "E:\writeback",
    "E:\"
)
foreach ($cd in $CowDirs) {
    if (Test-Path $cd) {
        Get-ChildItem -Path $cd -Filter "*.cow" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "    Removing stale overlay: $($_.FullName)" -ForegroundColor DarkYellow
            Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
        }
    }
}
# ---------------------------------------------------------------------------
# Step 1.5: Ensure CobDiskless Server is Running
# ---------------------------------------------------------------------------
$appProc = Get-Process "cobdiskless_app" -ErrorAction SilentlyContinue
if (-not $appProc) {
    $AppExe = "$WorkspaceRoot\build\bin\cobdiskless_app.exe"
    $AppWorkDir = "$WorkspaceRoot\build\bin"
    if (Test-Path $AppExe) {
        Write-Host "[*] Starting cobdiskless_app.exe (--start-daemon --enable-trace-io)..." -ForegroundColor Yellow
        $appPsi = New-Object System.Diagnostics.ProcessStartInfo
        $appPsi.FileName = $AppExe
        $appPsi.Arguments = "--start-daemon --enable-trace-io"
        $appPsi.WorkingDirectory = $AppWorkDir
        $appPsi.UseShellExecute = $false
        $appPsi.CreateNoWindow = $true
        $appProc = [System.Diagnostics.Process]::Start($appPsi)
        Start-Sleep -Seconds 2
        Write-Host "[+] cobdiskless_app started successfully (PID: $($appProc.Id))" -ForegroundColor Green
    }
} else {
    Write-Host "[+] cobdiskless_app already running (PID: $($appProc.Id))" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# C# Helper for PPM to PNG conversion and Screen Classification
# ---------------------------------------------------------------------------
$CSharpSource = @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public class ScreenAnalyzer {
    public static string ConvertAndClassify(string ppmPath, string pngPath) {
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
        
        // Skip whitespace and comments
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        while (idx < bytes.Length && bytes[idx] == '#') {
            while (idx < bytes.Length && bytes[idx] != '\n') idx++;
            while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        }
        
        // Width
        int width = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            width = width * 10 + (bytes[idx] - '0');
            idx++;
        }
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        
        // Height
        int height = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            height = height * 10 + (bytes[idx] - '0');
            idx++;
        }
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        
        // MaxVal
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
        long centerNonBlack = 0;
        
        int xMin = width / 4;
        int xMax = 3 * width / 4;
        int yMin = height / 4;
        int yMax = 3 * height / 4;
        
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
                    
                    row[x * 3]     = b; // B
                    row[x * 3 + 1] = g; // G
                    row[x * 3 + 2] = r; // R
                    
                    bool isBlack = (r < 18 && g < 18 && b < 18);
                    if (isBlack) {
                        blackPixels++;
                    } else {
                        if (x >= xMin && x <= xMax && y >= yMin && y <= yMax) {
                            centerNonBlack++;
                        }
                    }
                    
                    // Windows BSOD blue (#0078D7: R~0, G~120, B~215 or classic #000082)
                    if (b > 120 && b > r + 50 && (g > 60 || (r < 30 && g < 50))) {
                        bsodBluePixels++;
                    }
                    
                    // Text pixel
                    if (r > 160 && g > 160 && b > 160) {
                        textPixels++;
                    }
                }
                Marshal.Copy(row, 0, scan0 + y * stride, stride);
            }
            bmp.UnlockBits(data);
            bmp.Save(pngPath, ImageFormat.Png);
        }
        
        double blackRatio = (double)blackPixels / totalPixels;
        double bsodRatio = (double)bsodBluePixels / totalPixels;
        
        if (bsodRatio > 0.35) {
            return "BSOD_0x7B_DETECTED";
        }
        if (blackRatio > 0.998) {
            return "BLANK_BLACK_SCREEN";
        }
        if (textPixels > 80 && blackRatio > 0.88) {
            return "COBPXE_OR_BIOS_TEXT";
        }
        if (blackRatio > 0.75) {
            return "WINDOWS_BOOT_LOGO_OR_DOTS";
        }
        
        return "GRAPHICAL_SCREEN";
    }
}
'@

if (-not ([System.Management.Automation.PSTypeName]'ScreenAnalyzer').Type) {
    Add-Type -TypeDefinition $CSharpSource -ReferencedAssemblies System.Drawing
}

# ---------------------------------------------------------------------------
# Step 2: Start QEMU Headlessly with Custom CobPXE Option ROM
# ---------------------------------------------------------------------------
# Resolve custom CobPXE Option ROM
if (-not $RomFile) {
    $romCandidates = @(
        "$WorkspaceRoot\..\cobpxe\src\bin\$NicModel.rom",
        "$WorkspaceRoot\..\cobpxe\dist\$NicModel.rom",
        "$WorkspaceRoot\tftp_boot\$NicModel.rom",
        "$WorkspaceRoot\build\bin\tftp_boot\$NicModel.rom"
    )
    if ($NicModel -eq "e1000") {
        $romCandidates += "$WorkspaceRoot\..\cobpxe\src\bin\8086100e.rom"
        $romCandidates += "$WorkspaceRoot\..\cobpxe\dist\8086100e.rom"
    }
    foreach ($cand in $romCandidates) {
        if (Test-Path $cand) {
            $RomFile = (Resolve-Path $cand).Path
            break
        }
    }
}

$nicDevArg = "$NicModel,netdev=net0,mac=$Mac"
if ($RomFile -and (Test-Path $RomFile)) {
    $absRom = (Resolve-Path $RomFile).Path.Replace('\', '/')
    Write-Host "[+] Custom CobPXE Option ROM selected: $absRom" -ForegroundColor Green
    $nicDevArg += ",romfile=$absRom"
} else {
    Write-Warning "No custom Option ROM found; relying on default QEMU ROM."
}

Write-Host "[*] Launching QEMU headlessly..." -ForegroundColor Yellow
Write-Host "    NIC: $NicModel | MAC: $Mac | TAP: $TapAdapter | Device: $nicDevArg" -ForegroundColor DarkGray

$SerialLog = "$LogsDir\qemu_serial.log"
$GuestLog = "$LogsDir\qemu_guest.log"

# QEMU arguments
$qemuArgs = @(
    "-m", "$MemoryMb",
    "-smp", "$SmpCores",
    "-cpu", "max",
    "-accel", "tcg",
    "-display", "none",
    "-vga", "std",
    "-device", "$nicDevArg",
    "-netdev", "tap,id=net0,ifname=$TapAdapter",
    "-boot", "order=n",
    "-qmp", "tcp:127.0.0.1:$QmpPort,server,nowait",
    "-serial", "file:$SerialLog",
    "-d", "guest_errors",
    "-D", "$GuestLog"
)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $QemuPath
$psi.Arguments = [string]::Join(" ", ($qemuArgs | ForEach-Object { 
    if ($_ -match '\s') { "`"$_`"" } else { $_ } 
}))
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

$qemuProc = [System.Diagnostics.Process]::Start($psi)
if (-not $qemuProc -or $qemuProc.HasExited) {
    throw "Failed to start QEMU process. Check logs for details."
}
Write-Host "[+] QEMU started successfully (PID: $($qemuProc.Id))" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Step 3: Connect to QMP and Negotiate Capabilities
# ---------------------------------------------------------------------------
Write-Host "[*] Connecting to QMP on 127.0.0.1:$QmpPort..." -ForegroundColor Yellow
$tcpClient = $null
$stream = $null
$reader = $null
$writer = $null

$qmpConnected = $false
$connectAttempts = 0
while ($connectAttempts -lt 20 -and -not $qmpConnected) {
    $connectAttempts++
    try {
        $tcpClient = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $QmpPort)
        $stream = $tcpClient.GetStream()
        $reader = New-Object System.IO.StreamReader($stream)
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true
        
        # Read greeting banner
        $greeting = $reader.ReadLine()
        Write-Host "    QMP Banner: $greeting" -ForegroundColor DarkGray
        
        # Negotiate capabilities
        $writer.WriteLine('{"execute": "qmp_capabilities"}')
        $capResp = $reader.ReadLine()
        Write-Host "    QMP Capabilities Response: $capResp" -ForegroundColor DarkGray
        $qmpConnected = $true
    } catch {
        Start-Sleep -Milliseconds 500
    }
}

if (-not $qmpConnected) {
    Write-Warning "Could not connect to QMP server on port $QmpPort."
} else {
    Write-Host "[+] QMP connection established and capabilities negotiated." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Step 4: Screendump & Telemetry Loop
# ---------------------------------------------------------------------------
$StartTime = Get-Date
$EndTime = $StartTime.AddSeconds($DurationSeconds)
$Iteration = 0
$Timeline = @()

$AppLog = "$WorkspaceRoot\build\bin\cobdiskless.log"
$logPos = 0
if (Test-Path $AppLog) {
    $logPos = (Get-Item $AppLog).Length
}

Write-Host "[*] Monitoring boot progress for $DurationSeconds seconds (screendump every 5s)..." -ForegroundColor Yellow

try {
    while ((Get-Date) -lt $EndTime -and -not $qemuProc.HasExited) {
        $Iteration++
        $Elapsed = [int]((Get-Date) - $StartTime).TotalSeconds
        
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
                        if ($line -match '\[(BOOT|iSCSI|ISCSI|SCSI|WARN|ERROR)\]') {
                            Write-Host "    [SERVER] $line" -ForegroundColor DarkCyan
                        }
                    }
                } catch {}
            }
        }
        
        if ($qmpConnected) {
            $ppmPath = "$LogsDir\boot_screen.ppm"
            $pngPath = "$LogsDir\boot_screen_${Iteration}.png"
            $latestPng = "$LogsDir\boot_screen.png"
            
            # Use forward slashes for QMP path
            $qmpPpm = $ppmPath.Replace('\', '/')
            $cmd = '{"execute": "screendump", "arguments": {"filename": "' + $qmpPpm + '"}}'
            $writer.WriteLine($cmd)
            
            # Read response
            $resp = $reader.ReadLine()
            
            if (Test-Path $ppmPath) {
                $status = [ScreenAnalyzer]::ConvertAndClassify($ppmPath, $pngPath)
                Copy-Item $pngPath $latestPng -Force -ErrorAction SilentlyContinue
                
                $Timeline += [PSCustomObject]@{
                    Second = $Elapsed
                    Iteration = $Iteration
                    VisualState = $status
                    Png = $pngPath
                }
                
                Write-Host "    [T+${Elapsed}s] Screendump #${Iteration}: $status (Saved: boot_screen_${Iteration}.png)" -ForegroundColor $(
                    if ($status -eq "BSOD_0x7B_DETECTED") { "Red" }
                    elseif ($status -eq "WINDOWS_BOOT_LOGO_OR_DOTS") { "Cyan" }
                    elseif ($status -eq "COBPXE_OR_BIOS_TEXT") { "Magenta" }
                    elseif ($status -eq "GRAPHICAL_SCREEN") { "Green" }
                    else { "DarkGray" }
                )
                
                if ($status -eq "BSOD_0x7B_DETECTED") {
                    Write-Host "[!] BSOD 0x7B detected on screen! Continuing capture to confirm..." -ForegroundColor Red
                }
            }
        }
        
        Start-Sleep -Seconds 5
    }
} finally {
    if ($writer) { $writer.Close() }
    if ($reader) { $reader.Close() }
    if ($stream) { $stream.Close() }
    if ($tcpClient) { $tcpClient.Close() }
    
    if (-not $KeepVmRunning) {
        Write-Host "[*] Stopping QEMU process..." -ForegroundColor Yellow
        if (-not $qemuProc.HasExited) {
            Stop-Process -Id $qemuProc.Id -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "[*] Leaving QEMU VM running as requested (PID: $($qemuProc.Id))." -ForegroundColor Green
    }
}

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "Execution Summary" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
$Timeline | Format-Table -AutoSize

if (Test-Path $GuestLog) {
    $guestSize = (Get-Item $GuestLog).Length
    Write-Host "[*] QEMU Guest Log: $GuestLog ($guestSize bytes)" -ForegroundColor DarkGray
}
if (Test-Path $SerialLog) {
    $serialContent = Get-Content $SerialLog -ErrorAction SilentlyContinue
    if ($serialContent) {
        Write-Host "[*] QEMU Serial Log Tail:" -ForegroundColor DarkGray
        $serialContent | Select-Object -Last 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    }
}

if (Test-Path $AppLog) {
    Write-Host "[*] Recent CobDiskless Boot & SCSI Events:" -ForegroundColor Cyan
    Get-Content $AppLog -Tail 200 | Where-Object { $_ -match '\[(BOOT|iSCSI|ISCSI|SCSI)\]' } | Select-Object -Last 25 | ForEach-Object {
        Write-Host "    $_" -ForegroundColor DarkGray
    }
}

Write-Host "[+] Headless run finished." -ForegroundColor Green
