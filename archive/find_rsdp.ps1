$bytes = [System.IO.File]::ReadAllBytes('logs\low_mem.bin')
$rsdpSig = [System.Text.Encoding]::ASCII.GetBytes('RSD PTR ')
for ($i = 0; $i -lt ($bytes.Length - 16); $i += 16) {
    $match = $true
    for ($j = 0; $j -lt 8; $j++) {
        if ($bytes[$i + $j] -ne $rsdpSig[$j]) { $match = $false; break }
    }
    if ($match) {
        $rsdtAddr = [BitConverter]::ToUInt32($bytes, $i + 16)
        $xsdtAddr = [BitConverter]::ToUInt64($bytes, $i + 24)
        Write-Host ('RSDP found at 0x{0:X5}: RSDT=0x{1:X8}, XSDT=0x{2:X16}' -f $i, $rsdtAddr, $xsdtAddr)
    }
}
