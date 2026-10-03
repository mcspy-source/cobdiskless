$guid = '9f9d784a-7bc9-4da4-8b6b-4370a256d0a7' # Let's search catalog for KB4480116 first
$uri = 'https://www.catalog.update.microsoft.com/Search.aspx?q=KB4480116'
$r = Invoke-WebRequest -Uri $uri -UseBasicParsing
$guids = [regex]::Matches($r.Content, '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}') | ForEach-Object { $_.Value } | Select-Object -Unique

foreach ($g in $guids) {
    try {
        $post = @{ size = 0; updateID = $g; uidInfo = $g } | ConvertTo-Json -Compress
        $body = @{ updateIDs = "[$post]" }
        $resp = Invoke-WebRequest -Uri 'https://www.catalog.update.microsoft.com/DownloadDialog.aspx' -Method Post -Body $body -UseBasicParsing
        $urls = [regex]::Matches($resp.Content, 'http[s]?://[^\s''"]+\.msu') | ForEach-Object { $_.Value } | Select-Object -Unique
        foreach ($u in $urls) {
            Write-Host "KB4480116 URL: $u"
        }
    } catch {}
}
