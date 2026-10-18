$tcp = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 5555)
$s = $tcp.GetStream()
$r = New-Object System.IO.StreamReader($s)
$w = New-Object System.IO.StreamWriter($s)
$w.AutoFlush = $true
$r.ReadLine()
$w.WriteLine('{"execute":"qmp_capabilities"}')
$r.ReadLine()
$w.WriteLine('{"execute":"screendump","arguments":{"filename":"logs/local_screendump.ppm"}}')
$r.ReadLine()
$tcp.Close()

if (Test-Path "logs/local_screendump.ppm") {
    [double]$b = 0.0
    [double]$bs = 0.0
    $class = [ScreenAnalyzer]::ConvertAndClassify("logs/local_screendump.ppm", "logs/local_screendump.png", [ref]$b, [ref]$bs)
    Write-Host "Local Boot Screen: $class (Black: $([math]::Round($b*100, 1))%, BSOD: $([math]::Round($bs*100, 1))%)"
}
