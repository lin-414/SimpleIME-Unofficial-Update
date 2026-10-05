param([string]$Exe, [string]$ExeArgs, [string]$Out, [string]$Cursor, [string]$ErrLog)
Add-Type -AssemblyName System.Drawing
# Physical-pixel coordinates: match the DPI-aware preview host 1:1.
# Captures the CLIENT area so PNG pixels == ImGui coordinates (for --hover=).
# -Cursor "x,y" moves the real cursor to client coords so hover states render.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Cap {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }
}
"@
[Win32Cap]::SetProcessDPIAware() | Out-Null
if ([string]::IsNullOrEmpty($ExeArgs)) { $ExeArgs = " " }
if ([string]::IsNullOrEmpty($ErrLog)) {
    $p = Start-Process -FilePath $Exe -ArgumentList $ExeArgs -PassThru
} else {
    $p = Start-Process -FilePath $Exe -ArgumentList $ExeArgs -PassThru -RedirectStandardError $ErrLog
}
Start-Sleep -Milliseconds 3000
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 1500; $p.Refresh(); $h = $p.MainWindowHandle }
[Win32Cap]::ShowWindow($h, 9) | Out-Null
[Win32Cap]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 800
if (-not [string]::IsNullOrEmpty($Cursor)) {
    $parts = $Cursor.Split(',')
    $pt = New-Object Win32Cap+POINT
    $pt.X = [int]$parts[0]; $pt.Y = [int]$parts[1]
    [Win32Cap]::ClientToScreen($h, [ref]$pt) | Out-Null
    [Win32Cap]::SetCursorPos($pt.X, $pt.Y) | Out-Null
    Start-Sleep -Milliseconds 400
}
$rect = New-Object Win32Cap+RECT
[Win32Cap]::GetClientRect($h, [ref]$rect) | Out-Null
$origin = New-Object Win32Cap+POINT
$origin.X = 0; $origin.Y = 0
[Win32Cap]::ClientToScreen($h, [ref]$origin) | Out-Null
$w = $rect.Right; $hgt = $rect.Bottom
$bmp = New-Object System.Drawing.Bitmap($w, $hgt)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force
Write-Output "saved $Out ($w x $hgt)"
