$ErrorActionPreference = "Stop"

function Inspect-VhdFile([string]$vhdPath) {
    Write-Host "=========================================================="
    Write-Host "SURGICAL INSPECTION FOR: $vhdPath"
    Write-Host "=========================================================="

    if (-not (Test-Path $vhdPath)) {
        Write-Host "ERROR: File not found at $vhdPath" -ForegroundColor Red
        return
    }

    $fi = Get-Item $vhdPath
    Write-Host ("File Size: {0} bytes ({1:N2} MB)" -f $fi.Length, ($fi.Length / 1MB))

    $stream = [System.IO.File]::Open($vhdPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        function Read-BE32([byte[]]$buf, [int]$offset) {
            return [uint32](([uint32]$buf[$offset] -shl 24) -bor ([uint32]$buf[$offset + 1] -shl 16) -bor ([uint32]$buf[$offset + 2] -shl 8) -bor [uint32]$buf[$offset + 3])
        }
        function Read-BE64([byte[]]$buf, [int]$offset) {
            $h = Read-BE32 $buf $offset
            $l = Read-BE32 $buf ($offset + 4)
            return ([int64]$h -shl 32) -bor [int64]$l
        }

        # 1. First 512 bytes
        $stream.Seek(0, [System.IO.SeekOrigin]::Begin) | Out-Null
        $f0 = New-Object byte[] 512
        $stream.Read($f0, 0, 512) | Out-Null
        $cookie = [System.Text.Encoding]::ASCII.GetString($f0, 0, 8)
        $diskType = Read-BE32 $f0 60
        $dataOffset = Read-BE64 $f0 16
        Write-Host ("VHD Footer Cookie (0x00): '{0}'" -f $cookie)
        Write-Host ("VHD Disk Type (offset 60 / 0x3C): {0} ({1})" -f $diskType, $(switch($diskType){2{"Fixed"} 3{"Dynamic"} 4{"Differencing"} default{"Unknown"}}))
        Write-Host ("Data Offset (offset 16): {0} (0x{0:X})" -f $dataOffset)

        $isDynamic = ($diskType -eq 3 -or $diskType -eq 4)
        $bat = @()
        $blockSize = 0
        $sectorsPerBlock = 0
        $bitmapSectors = 0

        if ($isDynamic) {
            $stream.Seek($dataOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
            $dh = New-Object byte[] 1024
            $stream.Read($dh, 0, 1024) | Out-Null
            $dynCookie = [System.Text.Encoding]::ASCII.GetString($dh, 0, 8)
            $tableOffset = Read-BE64 $dh 16
            $maxEntries = Read-BE32 $dh 28
            $blockSize = Read-BE32 $dh 32
            $sectorsPerBlock = $blockSize / 512
            $bitmapBytes = [int][Math]::Ceiling($sectorsPerBlock / 8.0)
            $bitmapSectors = [int][Math]::Ceiling($bitmapBytes / 512.0)

            Write-Host ("Dynamic Header Cookie: '{0}', Table Offset: {1}, Max Entries: {2}" -f $dynCookie, $tableOffset, $maxEntries)
            Write-Host ("Block Size: {0} bytes ({1} sectors, {2} MB), Bitmap Sectors: {3}" -f $blockSize, $sectorsPerBlock, ($blockSize/1MB), $bitmapSectors)

            $stream.Seek($tableOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
            $rawBat = New-Object byte[] ($maxEntries * 4)
            $stream.Read($rawBat, 0, $rawBat.Length) | Out-Null
            $bat = New-Object uint32[] $maxEntries
            for ($i = 0; $i -lt $maxEntries; $i++) {
                $bat[$i] = Read-BE32 $rawBat ($i * 4)
            }
            Write-Host "BAT table read successfully."
        }

        function Read-RawBytes([int64]$fileOffset, [int]$count) {
            $buf = New-Object byte[] $count
            if ($fileOffset + $count -le $stream.Length) {
                $stream.Seek($fileOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
                $stream.Read($buf, 0, $count) | Out-Null
            }
            return $buf
        }

        function Read-VirtualSector([int64]$lba) {
            $buf = New-Object byte[] 512
            if (-not $isDynamic) {
                return Read-RawBytes ($lba * 512) 512
            } else {
                $blockIdx = [int][Math]::Floor($lba / $sectorsPerBlock)
                $secInBlock = [int]($lba % $sectorsPerBlock)
                if ($blockIdx -ge $bat.Length) { return $buf }
                $blockSector = $bat[$blockIdx]
                if ($blockSector -eq 0xFFFFFFFF) { return $buf }
                $fileSec = [int64]$blockSector + $bitmapSectors + $secInBlock
                return Read-RawBytes ($fileSec * 512) 512
            }
        }

        # 2. RAW FILE OFFSET (16346 * 512 = 8369152)
        Write-Host "`n--- RAW FILE OFFSET 8369152 (16346 * 512) ---"
        $raw16346 = Read-RawBytes 8369152 512
        $rawOem = [System.Text.Encoding]::ASCII.GetString($raw16346, 3, 8)
        $rawSig = [System.BitConverter]::ToUInt16($raw16346, 510)
        $rawHidden = [System.BitConverter]::ToUInt32($raw16346, 0x1C)
        $rawHex16 = ($raw16346[0..15] | ForEach-Object { "{0:X2}" -f $_ }) -join " "
        Write-Host ("Raw OEM ID        : '{0}'" -f $rawOem)
        Write-Host ("Raw Sig (0x1FE)   : 0x{0:X4}" -f $rawSig)
        Write-Host ("Raw Hidden Sectors: {0} (0x{0:X8})" -f $rawHidden)
        Write-Host ("Raw First 16 bytes: {0}" -f $rawHex16)

        # RAW FILE OFFSET (16347 * 512 = 8369664)
        Write-Host "`n--- RAW FILE OFFSET 8369664 (16347 * 512, IPL) ---"
        $raw16347 = Read-RawBytes 8369664 512
        $rawIplNonZero = ($raw16347 | Where-Object { $_ -ne 0 }).Count
        $rawIplHex16 = ($raw16347[0..15] | ForEach-Object { "{0:X2}" -f $_ }) -join " "
        Write-Host ("Raw Non-zero bytes: {0}/512" -f $rawIplNonZero)
        Write-Host ("Raw First 16 bytes: {0}" -f $rawIplHex16)

        # 3. VIRTUAL DISK MBR (Sector 0)
        Write-Host "`n--- VIRTUAL DISK MBR (LBA 0) ---"
        $mbr = Read-VirtualSector 0
        $mbrSig = [System.BitConverter]::ToUInt16($mbr, 510)
        Write-Host ("Virtual MBR Sig (0x1FE): 0x{0:X4}" -f $mbrSig)
        $detectedPartLBAs = @()
        for ($i = 0; $i -lt 4; $i++) {
            $off = 0x1BE + ($i * 16)
            $bFlag = $mbr[$off]
            $pType = $mbr[$off + 4]
            $sLBA = [System.BitConverter]::ToUInt32($mbr, $off + 8)
            $nSec = [System.BitConverter]::ToUInt32($mbr, $off + 12)
            Write-Host ("Part {0}: Boot=0x{1:X2}, Type=0x{2:X2}, StartLBA={3}, Size={4}" -f ($i+1), $bFlag, $pType, $sLBA, $nSec)
            if ($sLBA -gt 0) {
                $detectedPartLBAs += $sLBA
            }
        }

        # 4. Check each detected partition VBR & BPB
        foreach ($pLba in $detectedPartLBAs) {
            Write-Host ("`n--- VIRTUAL PARTITION VBR AT LBA {0} ---" -f $pLba)
            $vbr = Read-VirtualSector $pLba
            $vbrOem = [System.Text.Encoding]::ASCII.GetString($vbr, 3, 8)
            $vbrSig = [System.BitConverter]::ToUInt16($vbr, 510)
            $bps = [System.BitConverter]::ToUInt16($vbr, 0x0B)
            $spc = $vbr[0x0D]
            $hidden = [System.BitConverter]::ToUInt32($vbr, 0x1C)
            Write-Host ("OEM ID          : '{0}'" -f $vbrOem)
            Write-Host ("Signature       : 0x{0:X4}" -f $vbrSig)
            Write-Host ("Bytes Per Sector: {0}" -f $bps)
            Write-Host ("Sectors / Clust : {0}" -f $spc)
            Write-Host ("Hidden Sectors  : {0} (Matches LBA: {1})" -f $hidden, ($hidden -eq $pLba))

            # Inspect IPL Sector (VBR + 1)
            $ipl = Read-VirtualSector ($pLba + 1)
            $iplNonZero = ($ipl | Where-Object { $_ -ne 0 }).Count
            $iplHex = ($ipl[0..15] | ForEach-Object { "{0:X2}" -f $_ }) -join " "
            Write-Host ("IPL Sector LBA {0} Non-Zero Bytes: {1}/512" -f ($pLba + 1), $iplNonZero)
            Write-Host ("IPL First 16 bytes: {0}" -f $iplHex)
        }

        # 5. Check Virtual LBA 16346 specifically
        Write-Host "`n--- VIRTUAL LBA 16346 ---"
        $v16346 = Read-VirtualSector 16346
        $v16346Oem = [System.Text.Encoding]::ASCII.GetString($v16346, 3, 8)
        $v16346Sig = [System.BitConverter]::ToUInt16($v16346, 510)
        $v16346Hidden = [System.BitConverter]::ToUInt32($v16346, 0x1C)
        $v16346Hex = ($v16346[0..15] | ForEach-Object { "{0:X2}" -f $_ }) -join " "
        Write-Host ("Virtual 16346 OEM ID: '{0}', Sig: 0x{1:X4}, Hidden: {2}" -f $v16346Oem, $v16346Sig, $v16346Hidden)
        Write-Host ("Virtual 16346 First 16 bytes: {0}" -f $v16346Hex)

        # 6. Check Virtual LBA 16347 specifically
        $v16347 = Read-VirtualSector 16347
        $v16347NonZero = ($v16347 | Where-Object { $_ -ne 0 }).Count
        $v16347Hex = ($v16347[0..15] | ForEach-Object { "{0:X2}" -f $_ }) -join " "
        Write-Host ("Virtual 16347 Non-Zero Bytes: {0}/512" -f $v16347NonZero)
        Write-Host ("Virtual 16347 First 16 bytes: {0}" -f $v16347Hex)

        # 7. Check Virtual LBA 17946 (Recent WRITE (10) sector)
        $v17946 = Read-VirtualSector 17946
        $v17946NonZero = ($v17946 | Where-Object { $_ -ne 0 }).Count
        $v17946Hex = ($v17946[0..15] | ForEach-Object { "{0:X2}" -f $_ }) -join " "
        Write-Host "`n--- VIRTUAL LBA 17946 (Recent WRITE) ---"
        Write-Host ("Virtual 17946 Non-Zero Bytes: {0}/512" -f $v17946NonZero)
        Write-Host ("Virtual 17946 First 16 bytes: {0}" -f $v17946Hex)

    } finally {
        $stream.Close()
        $stream.Dispose()
    }
}

Inspect-VhdFile "c:\Users\Admin\Desktop\default.vhd"
Inspect-VhdFile "G:\w11.VHD"
