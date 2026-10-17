param(
    [string]$Path = 'logs\vm1_net.pcap',
    [double]$MinTime = 20.0,
    [double]$MaxTime = 90.0
)
$bytes = [System.IO.File]::ReadAllBytes($Path)
$off = 24
$first = $null
$found = 0
while ($off + 16 -le $bytes.Length) {
    $tsSec = [BitConverter]::ToUInt32($bytes, $off)
    $tsUsec = [BitConverter]::ToUInt32($bytes, $off+4)
    $incl = [BitConverter]::ToUInt32($bytes, $off+8)
    $off += 16
    if ($off + $incl -gt $bytes.Length) { break }
    if ($first -eq $null) { $first = $tsSec + $tsUsec/1e6 }
    $t = ($tsSec + $tsUsec/1e6) - $first
    if ($t -ge $MinTime -and $t -le $MaxTime) {
        $et = $bytes[$off+12]*256 + $bytes[$off+13]
        $srcMac = ($bytes[($off+6)..($off+11)] | ForEach-Object { $_.ToString('x2') }) -join ':'
        $dstMac = ($bytes[$off..($off+5)] | ForEach-Object { $_.ToString('x2') }) -join ':'
        $desc = "ethertype=0x{0:X4} len={1} src={2} dst={3}" -f $et, $incl, $srcMac, $dstMac
        if ($et -eq 0x0806) {
            $op = $bytes[$off+20]*256 + $bytes[$off+21]
            $spa = ($bytes[($off+28)..($off+31)] -join '.')
            $tpa = ($bytes[($off+38)..($off+41)] -join '.')
            $desc += " ARP op=$op spa=$spa tpa=$tpa"
        } elseif ($et -eq 0x0800) {
            $proto = $bytes[$off+23]
            $sip = ($bytes[($off+26)..($off+29)] -join '.')
            $dip = ($bytes[($off+30)..($off+33)] -join '.')
            $desc += " IP proto=$proto sip=$sip dip=$dip"
        }
        Write-Host ('[{0:N2}s] {1}' -f $t, $desc)
        $found++
        if ($found -ge 50) { break }
    }
    $off += $incl
}
Write-Host "Total frames in [$MinTime..$MaxTime]: $found"
