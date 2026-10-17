$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$res = @()
$res += (Get-ChildItem -Path "I:\" -Force | Out-String)
$res += (Get-ChildItem -Path "I:\Windows" -Force -ErrorAction SilentlyContinue | Out-String)
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\wdf_files_check.txt" -Encoding utf8
