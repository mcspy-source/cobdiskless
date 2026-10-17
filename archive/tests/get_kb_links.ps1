$uri = 'https://www.catalog.update.microsoft.com/Search.aspx?q=KB4580390'
$r = Invoke-WebRequest -Uri $uri -UseBasicParsing
$guids = [regex]::Matches($r.Content, '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}') | ForEach-Object { $_.Value } | Select-Object -Unique
Write-Host "Found GUIDs:"
$guids

foreach ($g in $guids) {
    try {
        $post = @{ size = 0; updateID = $g; uidInfo = $g } | ConvertTo-Json -Compress
        $body = @{ updateIDs = "[$post]" }
        $resp = Invoke-WebRequest -Uri 'https://www.catalog.update.microsoft.com/DownloadDialog.aspx' -Method Post -Body $body -UseBasicParsing
        $links = [regex]::Matches($resp.Content, 'http[s]?://download\.windowsupdate\.com/[^\''"]*') | ForEach-Object { $_.Value } | Select-Object -Unique
        foreach ($link in $links) {
            Write-Host "GUID $g -> $link"
        }
    } catch {
        # ignore invalid guid
    }
}
