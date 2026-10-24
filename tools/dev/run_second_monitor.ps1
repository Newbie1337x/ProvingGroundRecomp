# Starts the game windowed on a secondary monitor without taking the foreground from the
# window in use, captures the game window, then closes it.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [string[]]$GameArgs = @(),
    [Parameter(Mandatory)][string]$OutDir,
    [int[]]$At = @(60),
    # Window size; 0 keeps the game's own.
    [int]$Width = 0,
    [int]$Height = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PgPlace {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@
$screen = [System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1
if (-not $screen) { throw 'No secondary monitor' }
$previous = [PgPlace]::GetForegroundWindow()
$proc = Start-Process $Exe -ArgumentList (@('--fullscreen=false') + $GameArgs) -PassThru
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 100
}
if ($proc.MainWindowHandle -eq 0) { throw 'The game window did not appear' }
# SWP_NOZORDER | SWP_NOACTIVATE, and SWP_NOSIZE without a size
$flags = 0x0004 -bor 0x0010
if (-not $Width -or -not $Height) { $flags = $flags -bor 0x0001 }
$x = if ($Width) { $screen.Bounds.X } else { $screen.Bounds.X + 40 }
$y = if ($Height) { $screen.Bounds.Y } else { $screen.Bounds.Y + 40 }
[void][PgPlace]::SetWindowPos($proc.MainWindowHandle, [IntPtr]::Zero, $x, $y, $Width, $Height, $flags)
if ($previous -ne [IntPtr]::Zero) { [void][PgPlace]::SetForegroundWindow($previous) }
try {
    & (Join-Path $PSScriptRoot 'capture_window.ps1') -OutDir $OutDir -At $At
} finally {
    $alive = -not $proc.HasExited
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    "alive_at_end=$alive"
}
