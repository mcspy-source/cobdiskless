$guid = 'd4c8b6ac-dc65-499e-9158-9866cc02272c'
$postJson = '[{"size":0,"languages":"","uidInfo":"' + $guid + '","updateID":"' + $guid + '"}]'
$body = @{ updateIDs = $postJson }
$resp = Invoke-WebRequest -Uri 'https://www.catalog.update.microsoft.com/DownloadDialog.aspx' -Method Post -Body $body -UseBasicParsing
$resp.Content | Out-File "resp.html" -Encoding utf8
Write-Host "Response length: $($resp.Content.Length)"
$matches = [regex]::Matches($resp.Content, 'downloadInformation\[\d+\]\s*=\s*\{[^}]*\}')
foreach ($m in $matches) {
    Write-Host $m.Value
}
$urls = [regex]::Matches($resp.Content, 'http[s]?://[^\s''"]+\.msu')
foreach ($u in $urls) {
    Write-Host "URL: $($u.Value)"
}
