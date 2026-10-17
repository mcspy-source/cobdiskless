$j = Get-Content -Raw "msiscsi.sys.json" | ConvertFrom-Json
$withUrls = @()
foreach ($prop in $j.PSObject.Properties) {
    $val = $prop.Value
    if ($val.downloadUrl) {
        $withUrls += [PSCustomObject]@{
            Version = $val.fileInfo.version
            Url = $val.downloadUrl
        }
    }
}
Write-Host "Total with downloadUrl: $($withUrls.Count)"
$withUrls | Select-Object -First 20 | Format-Table -AutoSize
