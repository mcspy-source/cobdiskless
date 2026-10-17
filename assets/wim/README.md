# Static boot assets served at `/static/`

`build_winpe.ps1` publishes everything a client needs to boot the in-RAM WinPE
staging image into `wim/`:

| File        | Purpose                                                        |
|-------------|----------------------------------------------------------------|
| `wimboot`   | iPXE RAM-disk loader (the `kernel` of the wimboot script)       |
| `bootmgr`   | Legacy BIOS boot manager                                        |
| `bcd`       | Legacy BIOS BCD, retargeted to `\Windows\system32\boot\winload.exe` |
| `boot.sdi`  | RAM disk template                                               |
| `boot.wim`  | Compressed WinPE image (startnet.cmd + generic NIC drivers)     |

`GET /api/boot-script` hands an UNPROFILED or STAGING client an iPXE script that
pulls these five files over HTTP, so the machine stages its hardware without
ever touching its target VHD. Files are streamed from disk in 64 KiB chunks, so
`boot.wim` can be several hundred MB without growing the server's working set.

Build them with:

```powershell
.\scripts\wim\build_winpe.ps1 -DriverRoot D:\driver_pool -WimbootPath .\scripts\wim\wimboot
```

`wimboot` itself is not committed here - download it from
<https://github.com/ipxe/ipxe/releases> and pass `-WimbootPath`.
