# Grabs the BeamNG window straight off the desktop as fast as possible and counts Mario-red pixels around the
# middle of the screen in each grab - a blank frame shows up as a grab with (almost) no red. In-game screenshots
# re-render a fresh frame, so they can't see flicker; this sees what the player sees.
param([int]$Frames = 60, [string]$OutDir = "", [int]$LowBelow = 20)
$saved = 0
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  public struct RECT { public int L, T, R, B; }
}
"@
$p = Get-Process BeamNG.drive.x64 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
[W]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 700
$r = New-Object W+RECT
[W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
# region: most of the window (Mario can drift off-centre while the camera swings round); skips the HUD corners
$rx = $r.L + [int]($w * 0.15); $ry = $r.T + [int]($h * 0.2); $rw = [int]($w * 0.7); $rh = [int]($h * 0.72)
$bmp = New-Object System.Drawing.Bitmap $rw, $rh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$counts = @()
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($f = 0; $f -lt $Frames; $f++) {
  $g.CopyFromScreen($rx, $ry, 0, 0, $bmp.Size)
  $red = 0
  for ($y = 0; $y -lt $rh; $y += 6) {
    for ($x = 0; $x -lt $rw; $x += 6) {
      $c = $bmp.GetPixel($x, $y)
      if ($c.R -gt 150 -and $c.G -lt 70 -and $c.B -lt 70) { $red++ }
    }
  }
  $counts += $red
  if ($OutDir -and ($f -lt 3 -or ($red -le $LowBelow -and $saved -lt 6))) { $bmp.Save((Join-Path $OutDir "cap_$f.png")); if ($f -ge 3) { $saved++ } }
}
$ms = $sw.ElapsedMilliseconds
Write-Output ("MS_PER_GRAB " + [int]($ms / $Frames))
Write-Output ("COUNTS " + ($counts -join ","))
