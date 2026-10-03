$res = @()
foreach ($loc in @("C:\CCBoot", "C:\Users\Admin\Desktop", "G:\", "C:\Windows\WinSxS")) {
    if (Test-Path $loc) {
        $files = Get-ChildItem -Path $loc -Filter "msiscsi.sys" -Recurse -ErrorAction SilentlyContinue
        foreach ($f in $files) {
            $ver = (Get-Item $f.FullName).VersionInfo.FileVersion
            $res += [PSCustomObject]@{
                Path = $f.FullName
                Length = $f.Length
                Version = $ver
            }
        }
    }
}
$res | Format-Table -AutoSize | Out-String | Write-Host
