$ErrorActionPreference = "Continue"
Start-Transcript -Path "C:\Users\Admin\Desktop\cobdiskless\phys_out.txt" -Force

$diskStream = [System.IO.File]::Open("\\.\PhysicalDrive3", [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
try {
    $mbr = New-Object byte[] 512
    $diskStream.Read($mbr, 0, 512) | Out-Null
    $sig = [BitConverter]::ToUInt16($mbr, 510)
    Write-Host ("PhysicalDrive3 MBR Sig: 0x{0:X4}" -f $sig)
    for ($i = 0; $i -lt 4; $i++) {
        $off = 0x1BE + ($i * 16)
        $boot = $mbr[$off]
        $type = $mbr[$off + 4]
        $start = [BitConverter]::ToUInt32($mbr, $off + 8)
        $len = [BitConverter]::ToUInt32($mbr, $off + 12)
        Write-Host ("Part {0}: Boot=0x{1:X2} Type=0x{2:X2} StartLBA={3} Len={4}" -f $i, $boot, $type, $start, $len)
    }
    
    # Check LBA 2048
    $diskStream.Seek([int64]2048 * 512, [System.IO.SeekOrigin]::Begin) | Out-Null
    $vbr2048 = New-Object byte[] 512
    $diskStream.Read($vbr2048, 0, 512) | Out-Null
    $sig2048 = [BitConverter]::ToUInt16($vbr2048, 510)
    $oem2048 = [System.Text.Encoding]::ASCII.GetString($vbr2048, 3, 8)
    $bps2048 = [BitConverter]::ToUInt16($vbr2048, 0x0B)
    $spc2048 = $vbr2048[0x0D]
    $hid2048 = [BitConverter]::ToUInt32($vbr2048, 0x1C)
    Write-Host ("`nLBA 2048: Sig=0x{0:X4} OEM='{1}' BytesPerSec={2} SecPerClus={3} HiddenSec={4}" -f $sig2048, $oem2048, $bps2048, $spc2048, $hid2048)

    # Check LBA 16346
    $diskStream.Seek([int64]16346 * 512, [System.IO.SeekOrigin]::Begin) | Out-Null
    $vbr16346 = New-Object byte[] 512
    $diskStream.Read($vbr16346, 0, 512) | Out-Null
    $sig16346 = [BitConverter]::ToUInt16($vbr16346, 510)
    $oem16346 = [System.Text.Encoding]::ASCII.GetString($vbr16346, 3, 8)
    $bps16346 = [BitConverter]::ToUInt16($vbr16346, 0x0B)
    $spc16346 = $vbr16346[0x0D]
    $hid16346 = [BitConverter]::ToUInt32($vbr16346, 0x1C)
    Write-Host ("LBA 16346: Sig=0x{0:X4} OEM='{1}' BytesPerSec={2} SecPerClus={3} HiddenSec={4}" -f $sig16346, $oem16346, $bps16346, $spc16346, $hid16346)

    # Check Volume H: directly
    $volH = [System.IO.File]::Open("\\.\H:", [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $vbrH = New-Object byte[] 512
    $volH.Read($vbrH, 0, 512) | Out-Null
    $volH.Close()
    Write-Host ("`nVolume H: VBR: Sig=0x{0:X4} OEM='{1}' BytesPerSec={2} SecPerClus={3} HiddenSec={4}" -f ([BitConverter]::ToUInt16($vbrH, 510)), [System.Text.Encoding]::ASCII.GetString($vbrH, 3, 8), [BitConverter]::ToUInt16($vbrH, 0x0B), $vbrH[0x0D], [BitConverter]::ToUInt32($vbrH, 0x1C))

} finally {
    $diskStream.Close()
    Stop-Transcript
}
