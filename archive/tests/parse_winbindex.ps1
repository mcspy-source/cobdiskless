$j = Get-Content -Raw "msiscsi.sys.json" | ConvertFrom-Json
$matches = @()
foreach ($prop in $j.PSObject.Properties) {
    $val = $prop.Value
    $ver = $val.fileInfo.version
    if ($ver -like "*17763*") {
        $matches += [PSCustomObject]@{
            Hash = $prop.Name
            Version = $ver
            Timestamp = $val.fileInfo.timestamp
            Size = $val.fileInfo.size
            Machine = $val.fileInfo.machineType
            Urls = $val.downloadUrl
        }
    }
}
$matches | Format-Table -AutoSize | Out-String | Write-Host
$matches | ConvertTo-Json -Depth 5 | Out-File "winbindex_17763.json" -Encoding utf8
