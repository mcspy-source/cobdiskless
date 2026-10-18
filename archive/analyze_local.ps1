param(
    [string]$Ppm = "logs/local_screendump.ppm",
    [string]$Png = "logs/local_screendump.png"
)

$CSharpSource = @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;

public class ScreenAnalyzer2 {
    public static string ConvertAndClassify(string ppmPath, string pngPath, out double blackRatio, out double bsodRatio) {
        blackRatio = 1.0;
        bsodRatio = 0.0;
        if (!File.Exists(ppmPath)) return "FILE_NOT_FOUND";
        byte[] bytes = File.ReadAllBytes(ppmPath);
        if (bytes.Length < 16) return "INVALID_PPM";
        
        int idx = 0;
        if (bytes[idx++] != 'P' || bytes[idx++] != '6') return "NOT_P6_PPM";
        
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        while (idx < bytes.Length && bytes[idx] == '#') {
            while (idx < bytes.Length && bytes[idx] != '\n') idx++;
            while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        }
        
        int width = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            width = width * 10 + (bytes[idx] - '0');
            idx++;
        }
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        
        int height = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            height = height * 10 + (bytes[idx] - '0');
            idx++;
        }
        while (idx < bytes.Length && (bytes[idx] == ' ' || bytes[idx] == '\t' || bytes[idx] == '\r' || bytes[idx] == '\n')) idx++;
        
        int maxval = 0;
        while (idx < bytes.Length && bytes[idx] >= '0' && bytes[idx] <= '9') {
            maxval = maxval * 10 + (bytes[idx] - '0');
            idx++;
        }
        if (idx < bytes.Length && (bytes[idx] == '\n' || bytes[idx] == ' ' || bytes[idx] == '\r')) idx++;
        
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return "INVALID_DIMENSIONS";
        int pixelDataLen = width * height * 3;
        if (idx + pixelDataLen > bytes.Length) return "TRUNCATED_PIXELS";
        
        long totalPixels = width * height;
        long blackPixels = 0;
        long bsodBluePixels = 0;
        
        using (Bitmap bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb)) {
            BitmapData data = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            int stride = data.Stride;
            byte[] row = new byte[stride];
            IntPtr scan0 = data.Scan0;
            
            for (int y = 0; y < height; y++) {
                int rowOffset = idx + y * width * 3;
                for (int x = 0; x < width; x++) {
                    int p = rowOffset + x * 3;
                    byte r = bytes[p];
                    byte g = bytes[p + 1];
                    byte b = bytes[p + 2];
                    
                    row[x * 3]     = b;
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = r;
                    
                    if (r < 25 && g < 25 && b < 25) {
                        blackPixels++;
                    } else if (b > 100 && b > (r * 1.5) && b > (g * 1.2)) {
                        bsodBluePixels++;
                    }
                }
                System.Runtime.InteropServices.Marshal.Copy(row, 0, scan0 + y * stride, stride);
            }
            bmp.UnlockBits(data);
            bmp.Save(pngPath, ImageFormat.Png);
        }
        
        blackRatio = (double)blackPixels / (double)totalPixels;
        bsodRatio = (double)bsodBluePixels / (double)totalPixels;
        
        if (bsodRatio > 0.35) return "BSOD_0x7B_DETECTED";
        if (blackRatio < 0.65) return "WINDOWS_DESKTOP_OR_UI";
        if (blackRatio > 0.95 && bsodRatio < 0.05) return "WINDOWS_BOOT_LOGO_OR_DOTS";
        return "UNKNOWN_SCREEN";
    }
}
'@

Add-Type -TypeDefinition $CSharpSource -ReferencedAssemblies System.Drawing

[double]$b = 0.0
[double]$bs = 0.0
$outPng = [System.IO.Path]::GetFullPath($Png)
$class = [ScreenAnalyzer2]::ConvertAndClassify((Resolve-Path $Ppm).Path, $outPng, [ref]$b, [ref]$bs)
Write-Host "Screen: $class (Black: $([math]::Round($b*100, 1))%, BSOD: $([math]::Round($bs*100, 1))%)"
