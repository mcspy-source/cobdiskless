try {
    $res = Mount-DiskImage -ImagePath "G:\G\csmw10.VHD" -PassThru -ErrorAction Stop
    "Mounted successfully"
    $res | Format-List *
    Get-Disk
    Get-Volume
} catch {
    $_.Exception | Format-List *
}
