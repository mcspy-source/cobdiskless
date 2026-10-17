param(
    [string]$FrameName,
    [string]$OutFileName = "vm1_live_800x600.png"
)
Add-Type -AssemblyName System.Drawing
if ($FrameName) {
    $latest = Get-Item -Path "$PSScriptRoot\..\logs\$FrameName" -ErrorAction SilentlyContinue
} else {
    $latest = Get-ChildItem -Path "$PSScriptRoot\..\logs" -Filter "vm1_frame_*.png" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if ($latest) {
    $bytes = [System.IO.File]::ReadAllBytes($latest.FullName)
    $ms = New-Object System.IO.MemoryStream(,$bytes)
    $b = [System.Drawing.Bitmap]::FromStream($ms)
    $t = New-Object System.Drawing.Bitmap 800, 600
    $g = [System.Drawing.Graphics]::FromImage($t)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($b, 0, 0, 800, 600)
    $outPath = "C:\Users\Admin\.gemini\antigravity\brain\81c2d51f-806a-4232-925f-cf02fdceae3a\$OutFileName"
    $t.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $t.Dispose()
    $b.Dispose()
    $ms.Dispose()
    Write-Host "Resized $($latest.Name) to $outPath"
} else {
    Write-Host "No frames found."
}
