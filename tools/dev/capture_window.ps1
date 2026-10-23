# Saves PNG captures of the game window only (PrintWindow, never the whole screen) while a
# route replays, to check rendering after backend changes.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutDir,
    # Seconds after the game window appears.
    [int[]]$At = @(40, 50, 60),
    [string]$ProcessName = 'ProvingGround',
    [int]$WaitSeconds = 120
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PgWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    // The visible top-level window of the process with the largest client area.
    public static IntPtr Find(uint pid) {
        IntPtr best = IntPtr.Zero; long bestArea = 0;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            RECT r;
            if (p == pid && IsWindowVisible(h) && GetClientRect(h, out r)) {
                long a = (long)(r.R - r.L) * (r.B - r.T);
                if (a > bestArea) { bestArea = a; best = h; }
            }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
'@
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$proc = $null
while (-not $proc -and (Get-Date) -lt $deadline) {
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $proc) { Start-Sleep -Milliseconds 500 }
}
if (-not $proc) { throw "No $ProcessName window appeared" }
$start = Get-Date
foreach ($t in ($At | Sort-Object)) {
    $wait = $t - ((Get-Date) - $start).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    $proc.Refresh()
    if ($proc.HasExited) { break }
    $h = [PgWin]::Find([uint32]$proc.Id)
    $r = New-Object PgWin+RECT
    [void][PgWin]::GetClientRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    [void][PgWin]::PrintWindow($h, $hdc, 3)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $OutDir ("t{0:D3}.png" -f $t)), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
