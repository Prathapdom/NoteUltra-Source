# Captures a PNG of NoteUltra for visual checks of the UI.
# Works on a locked/remote desktop because it uses PrintWindow instead of screen capture.
# Example:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\Screenshot.ps1 -File .\main.cpp -Out $env:TEMP\dark.png -Theme 2 -Commands 1202
# -Theme: 0 system, 1 light, 2 dark.  -Commands: comma-separated WM_COMMAND ids from the
# Command enum in main.cpp (e.g. 1202 Replace bar, 1305 Split, 1301 Wrap).
# The user's settings.ini is backed up and restored byte-for-byte afterwards.
param([string]$File,[string]$Out,[int]$Theme=2,[string]$Commands='',[string]$Exe=(Join-Path $PSScriptRoot '..\dist\NoteUltra.exe'))
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class W{
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
 public struct RECT{public int L,T,R,B;}
}
'@
$dir="$env:LOCALAPPDATA\NoteUltra";$ini="$dir\settings.ini";$backup="$ini.screenshot-backup";$had=Test-Path $ini
New-Item -ItemType Directory -Force $dir|Out-Null
if($had){Copy-Item $ini $backup -Force}
"[View]`r`nTheme=$Theme`r`nWrap=0`r`nFontSize=11`r`n"|Set-Content -Encoding Unicode $ini
$p=$null
try{
 $p=Start-Process $Exe -ArgumentList ('"'+(Resolve-Path $File)+'"') -PassThru
 Start-Sleep -Milliseconds 1500
 $p.Refresh();$h=$p.MainWindowHandle
 [W]::MoveWindow($h,40,40,1280,800,$true)|Out-Null
 if($Commands){foreach($c in $Commands.Split(',')){[W]::SendMessage($h,0x111,[IntPtr][int]$c,[IntPtr]::Zero)|Out-Null;Start-Sleep -Milliseconds 300}}
 Start-Sleep -Milliseconds 700
 $r=New-Object W+RECT;[W]::GetWindowRect($h,[ref]$r)|Out-Null
 $bmp=New-Object System.Drawing.Bitmap ($r.R-$r.L),($r.B-$r.T)
 $g=[System.Drawing.Graphics]::FromImage($bmp);$dc=$g.GetHdc();[W]::PrintWindow($h,$dc,2)|Out-Null;$g.ReleaseHdc($dc);$g.Dispose()
 $bmp.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png);$bmp.Dispose()
}finally{
 if($p -and !$p.HasExited){$p.Kill();$p.WaitForExit(5000)|Out-Null}
 if($had){Copy-Item $backup $ini -Force;Remove-Item $backup}else{Remove-Item $ini -ErrorAction SilentlyContinue}
}
