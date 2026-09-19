# sim_probe.ps1 — drive and screenshot the LIVE running sim window.
# Run under Windows PowerShell (powershell.exe) for System.Drawing / GDI.
#
#   powershell.exe -File tools\sim_probe.ps1 -Shot out.png
#   powershell.exe -File tools\sim_probe.ps1 -Tap "233,150" -Shot out.png
#   powershell.exe -File tools\sim_probe.ps1 -Swipe "10,233,300,233" -Shot out.png
#
# Coords are client pixels (0..466). PostMessage delivers input without
# needing to focus/raise the window; PrintWindow captures even if occluded.
param([string]$Tap, [string]$Swipe, [string]$Shot)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
using System.Drawing; using System.Drawing.Imaging;
public class SimIO {
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  const uint DOWN=0x201, UP=0x202, MOVE=0x200;
  static IntPtr LP(int x,int y){ return (IntPtr)((y<<16)|(x&0xFFFF)); }
  public static void Down(IntPtr h,int x,int y){ PostMessage(h,DOWN,(IntPtr)1,LP(x,y)); }
  public static void Move(IntPtr h,int x,int y){ PostMessage(h,MOVE,(IntPtr)1,LP(x,y)); }
  public static void Up(IntPtr h,int x,int y){ PostMessage(h,UP,(IntPtr)0,LP(x,y)); }
  public static void Grab(IntPtr h,string p){
    RECT r; GetWindowRect(h,out r);
    using(var b=new Bitmap(r.R-r.L,r.B-r.T)) using(var g=Graphics.FromImage(b)){
      IntPtr d=g.GetHdc(); PrintWindow(h,d,2); g.ReleaseHdc(d); b.Save(p,ImageFormat.Png);
    }
  }
}
"@ -ReferencedAssemblies System.Drawing

$h = (Get-Process sim -ErrorAction Stop | Select-Object -First 1).MainWindowHandle
if ($Tap)   { $c=$Tap -split ','; [SimIO]::Down($h,[int]$c[0],[int]$c[1]); [SimIO]::Up($h,[int]$c[0],[int]$c[1]); Start-Sleep -Milliseconds 350 }
if ($Swipe) {
  $c=$Swipe -split ','; $x0=[int]$c[0]; $y0=[int]$c[1]; $x1=[int]$c[2]; $y1=[int]$c[3]
  [SimIO]::Down($h,$x0,$y0); Start-Sleep -Milliseconds 20
  for ($i=1; $i -le 8; $i++) {
    [SimIO]::Move($h, [int]($x0+($x1-$x0)*$i/8), [int]($y0+($y1-$y0)*$i/8)); Start-Sleep -Milliseconds 18
  }
  [SimIO]::Up($h,$x1,$y1); Start-Sleep -Milliseconds 500
}
if ($Shot) { [SimIO]::Grab($h,$Shot); Write-Output "shot -> $Shot" }
