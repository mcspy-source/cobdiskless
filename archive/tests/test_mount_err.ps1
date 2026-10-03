try {
    $m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru -ErrorAction Stop
    $out = "Mount SUCCESS: Attached=$($m.Attached)"
    Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
} catch {
    $out = "Mount FAILED: $($_.Exception.Message)"
}
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_mount_error.txt" -Encoding utf8
