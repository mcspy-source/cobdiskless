$j = Get-Content -Raw "msiscsi.sys.json" | ConvertFrom-Json
$item = $j."99cdbd8c0891bfdd8d41385a3b16a824bffa2593f8f51eedf33a3f47e1feea6b"
$item | ConvertTo-Json -Depth 5 | Out-File "item_1554.json" -Encoding utf8
$item | Format-List
