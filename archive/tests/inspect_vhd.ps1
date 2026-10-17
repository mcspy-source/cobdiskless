$ErrorActionPreference = "Stop"
$vhdPath = "G:\w11.VHD"

Write-Host "=========================================="
Write-Host "INSPECTING VHD: $vhdPath"
Write-Host "=========================================="

if (-not (Test-Path $vhdPath)) {
    Write-Error "File not found: $vhdPath"
    exit 1
}

$fileInfo = Get-Item $vhdPath
Write-Host ("File Size: {0} bytes ({1:N2} MB)" -f $fileInfo.Length, ($fileInfo.Length / 1MB))

function Read-BEUInt16([byte[]]$buf, [int]$offset) {
    return [uint16](($buf[$offset] -shl 8) -bor $buf[$offset + 1])
}

function Read-BEUInt32([byte[]]$buf, [int]$offset) {
    return [uint32](([uint32]$buf[$offset] -shl 24) -bor ([uint32]$buf[$offset + 1] -shl 16) -bor ([uint32]$buf[$offset + 2] -shl 8) -bor [uint32]$buf[$offset + 3])
}

function Read-BEUInt64([byte[]]$buf, [int]$offset) {
    $high = Read-BEUInt32 $buf $offset
    $low  = Read-BEUInt32 $buf ($offset + 4)
    return ([int64]$high -shl 32) -bor [int64]$low
}

$stream = [System.IO.File]::Open($vhdPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
try {
    # Check VHD Footer
    $stream.Seek(0, [System.IO.SeekOrigin]::Begin) | Out-Null
    $footer = New-Object byte[] 512
    $stream.Read($footer, 0, 512) | Out-Null
    $footerCookie = [System.Text.Encoding]::ASCII.GetString($footer, 0, 8)
    
    $isDynamic = $false
    $bat = @()
    $blockSize = 0
    $sectorsPerBlock = 0

    if ($footerCookie -eq "conectix") {
        $diskTypeBE = Read-BEUInt32 $footer 60
        $dataOffset = Read-BEUInt64 $footer 16
        Write-Host ("VHD Cookie: '{0}'" -f $footerCookie)
        Write-Host ("VHD Disk Type: {0} (2=Fixed, 3=Dynamic, 4=Differencing)" -f $diskTypeBE)
        Write-Host ("Dynamic Header Offset: {0} (0x{0:X})" -f $dataOffset)

        if ($diskTypeBE -eq 3 -or $diskTypeBE -eq 4) {
            $isDynamic = $true
            # Read Dynamic Header
            $stream.Seek($dataOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
            $dynHeader = New-Object byte[] 1024
            $stream.Read($dynHeader, 0, 1024) | Out-Null
            $dynCookie = [System.Text.Encoding]::ASCII.GetString($dynHeader, 0, 8)
            Write-Host ("Dynamic Header Cookie: '{0}'" -f $dynCookie)

            $tableOffset = Read-BEUInt64 $dynHeader 16
            $maxEntries = Read-BEUInt32 $dynHeader 28
            $blockSize = Read-BEUInt32 $dynHeader 32
            $sectorsPerBlock = $blockSize / 512

            Write-Host ("BAT Table Offset: {0} (0x{0:X})" -f $tableOffset)
            Write-Host ("Max BAT Entries : {0}" -f $maxEntries)
            Write-Host ("Block Size      : {0} bytes ({1} sectors, {2} MB)" -f $blockSize, $sectorsPerBlock, ($blockSize/1MB))

            # Read BAT
            $stream.Seek($tableOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
            $batBytes = New-Object byte[] ($maxEntries * 4)
            $stream.Read($batBytes, 0, $batBytes.Length) | Out-Null
            $bat = New-Object uint32[] $maxEntries
            for ($i = 0; $i -lt $maxEntries; $i++) {
                $bat[$i] = Read-BEUInt32 $batBytes ($i * 4)
            }
            Write-Host "BAT loaded successfully."
        }
    }

    # Function to read virtual sector
    function Read-VirtualSector([int64]$lba) {
        $buf = New-Object byte[] 512
        if (-not $isDynamic) {
            $offset = $lba * 512
            $stream.Seek($offset, [System.IO.SeekOrigin]::Begin) | Out-Null
            $stream.Read($buf, 0, 512) | Out-Null
            return $buf
        } else {
            $blockIdx = [int][Math]::Floor($lba / $sectorsPerBlock)
            $sectorInBlock = [int]($lba % $sectorsPerBlock)
            if ($blockIdx -ge $bat.Length) { return $buf }
            $blockSectorOffset = $bat[$blockIdx]
            if ($blockSectorOffset -eq 0xFFFFFFFF) {
                # Unallocated block (zeros)
                return $buf
            }
            $bitmapBytes = [int][Math]::Ceiling($sectorsPerBlock / 8.0)
            $bitmapSectors = [int][Math]::Ceiling($bitmapBytes / 512.0)
            
            $fileSector = [int64]$blockSectorOffset + $bitmapSectors + $sectorInBlock
            $fileOffset = $fileSector * 512
            $stream.Seek($fileOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
            $stream.Read($buf, 0, 512) | Out-Null
            return $buf
        }
    }

    # 1. Sector 0 (MBR)
    $mbr = Read-VirtualSector 0
    $sig = [System.BitConverter]::ToUInt16($mbr, 510)
    Write-Host "`n=========================================="
    Write-Host ("MBR Signature (0x1FE): 0x{0:X4} (Expected 0xAA55)" -f $sig)

    Write-Host "`n--- MBR PARTITION TABLE (Sector 0) ---"
    $partitions = @()
    for ($i = 0; $i -lt 4; $i++) {
        $offset = 0x1BE + ($i * 16)
        $bootFlag = $mbr[$offset]
        $partType = $mbr[$offset + 4]
        $startLBA = [System.BitConverter]::ToUInt32($mbr, $offset + 8)
        $sizeSectors = [System.BitConverter]::ToUInt32($mbr, $offset + 12)
        $sizeMB = ($sizeSectors * 512.0) / 1MB

        $statusStr = if ($bootFlag -eq 0x80) { "BOOTABLE (0x80)" } elseif ($bootFlag -eq 0x00) { "Non-bootable (0x00)" } else { ("Unknown (0x{0:X2})" -f $bootFlag) }
        $typeStr = switch ($partType) {
            0x07 { "NTFS / exFAT / HPFS (0x07)" }
            0xEE { "GPT Protective (0xEE)" }
            0xEF { "EFI System Partition (0xEF)" }
            0x00 { "Empty (0x00)" }
            Default { "0x{0:X2}" -f $partType }
        }

        Write-Host ("Partition {0}:" -f ($i + 1))
        Write-Host ("  Status/Bootable Flag: {0}" -f $statusStr)
        Write-Host ("  Partition Type      : {0}" -f $typeStr)
        Write-Host ("  Starting LBA        : {0}" -f $startLBA)
        Write-Host ("  Size (Sectors)      : {0} ({1:N2} MB)" -f $sizeSectors, $sizeMB)

        if ($partType -ne 0) {
            $partitions += @{ Index = ($i + 1); StartLBA = $startLBA; BootFlag = $bootFlag }
        }
    }

    # Inspect VBR function
    function Inspect-VBR([int64]$lba, [string]$label) {
        Write-Host ("`n--- VBR INSPECTION: {0} (LBA {1}) ---" -f $label, $lba)
        $vbr = Read-VirtualSector $lba

        $vbrSig = [System.BitConverter]::ToUInt16($vbr, 510)
        $oemID = [System.Text.Encoding]::ASCII.GetString($vbr, 3, 8)
        $bytesPerSector = [System.BitConverter]::ToUInt16($vbr, 0x0B)
        $sectorsPerCluster = $vbr[0x0D]
        $reservedSectors = [System.BitConverter]::ToUInt16($vbr, 0x0E)
        $hiddenSectors = [System.BitConverter]::ToUInt32($vbr, 0x1C)
        $totalSectors = [System.BitConverter]::ToUInt64($vbr, 0x28)

        Write-Host ("OEM ID                : '{0}'" -f $oemID)
        Write-Host ("VBR Signature (0x1FE) : 0x{0:X4} (Expected 0xAA55)" -f $vbrSig)
        Write-Host ("Bytes Per Sector      : {0} (offset 0x0B)" -f $bytesPerSector)
        Write-Host ("Sectors Per Cluster   : {0} (offset 0x0D)" -f $sectorsPerCluster)
        Write-Host ("Reserved Sectors      : {0} (offset 0x0E)" -f $reservedSectors)
        Write-Host ("Hidden Sectors        : {0} (offset 0x1C, 32-bit uint)" -f $hiddenSectors)
        Write-Host ("Total Sectors in Vol  : {0}" -f $totalSectors)

        Write-Host "`n--- VERIFICATION CHECK ---"
        Write-Host ("Actual Starting LBA   : {0}" -f $lba)
        Write-Host ("BPB Hidden Sectors    : {0}" -f $hiddenSectors)

        if ($hiddenSectors -eq $lba) {
            Write-Host ("RESULT: Hidden Sectors MATCHES Starting LBA ({0})!" -f $lba) -ForegroundColor Green
        } else {
            Write-Host ("RESULT: MISMATCH DETECTED! Hidden Sectors = {0}, Expected = {1}" -f $hiddenSectors, $lba) -ForegroundColor Red
            Write-Host "EXPLANATION: The NTFS IPL calculates disk access using the BPB Hidden Sectors value. If Hidden Sectors does not match the actual partition starting LBA, the IPL halts before reading `$MFT or BOOTMGR." -ForegroundColor Yellow
        }
    }

    # Inspect requested LBA 16346
    Inspect-VBR 16346 "Requested Sector (LBA 16346)"

    # Inspect Partition 1 (Boot Partition, LBA 2048)
    Inspect-VBR 2048 "Partition 1 Boot Partition (System Reserved)"

    # Inspect Partition 2 (OS Partition, LBA 104448)
    Inspect-VBR 104448 "Partition 2 OS Partition (Windows X-Lite)"

} finally {
    $stream.Close()
    $stream.Dispose()
}
