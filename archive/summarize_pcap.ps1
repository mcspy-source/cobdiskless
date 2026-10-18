$b = [System.IO.File]::ReadAllBytes("logs\vm1.pcap")
Write-Host "pcap bytes: $($b.Length)"
$p = 24; $t0 = $null; $n = 0
$summary = @{}
while ($p + 16 -le $b.Length) {
    $sec = [BitConverter]::ToUInt32($b, $p); $us = [BitConverter]::ToUInt32($b, $p+4)
    $len = [BitConverter]::ToUInt32($b, $p+8)
    $d = $p + 16
    if ($d + $len -gt $b.Length) { break }
    $t = $sec + $us/1e6; if ($t0 -eq $null) { $t0 = $t }
    $rel = [int]($t - $t0)
    if ($len -ge 14) {
        $src = (6..11 | % { $b[$d+$_].ToString('X2') }) -join ':'
        $et = ($b[$d+12] * 256 + $b[$d+13])
        $kind = 'eth{0:X4}' -f $et
        if ($et -eq 0x0806) { $kind = 'ARP' }
        if ($et -eq 0x0800) {
            $pr = $b[$d+23]; $ihl = ($b[$d+14] -band 15) * 4
            $kind = "IP proto $pr"
            if ($pr -eq 6) { $dp = $b[$d+14+$ihl+2]*256 + $b[$d+14+$ihl+3]; $kind = "TCP dport $dp" }
            if ($pr -eq 17) { $dp = $b[$d+14+$ihl+2]*256 + $b[$d+14+$ihl+3]; $kind = "UDP dport $dp" }
        }
        $bucket = [int]([math]::Floor($rel / 10) * 10)
        $key = "T+${bucket}s $src $kind"
        $summary[$key] = 1 + [int]$summary[$key]
    }
    $p = $d + $len
}
$summary.GetEnumerator() | Sort-Object Name | % { "$($_.Name)  x$($_.Value)" }
