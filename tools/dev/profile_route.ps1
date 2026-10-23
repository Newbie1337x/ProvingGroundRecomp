# Unattended profiling run: replays a recorded route with Tracy enabled, captures the
# trace without the viewer, closes the game at measure_stop and writes a report.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Route,
    [switch]$NoVsync,
    # Counters only: no Tracy client, capture or trace analysis (cheaper, for A/B runs).
    [switch]$NoTracy,
    # Also write a sampling CPU profile of all threads (samples.txt) for the window.
    [switch]$Sample,
    # Also write timeline.csv: game-thread and GPU-thread wait events in the window.
    [switch]$Timeline,
    # Appended to the output directory name, e.g. "base" or "change1".
    [string]$Label = '',
    # By default the game runs muted, in a small window on -Monitor, without taking
    # focus, so the PC stays usable during long sessions. -Visible restores fullscreen.
    [switch]$Visible,
    [int]$Monitor = 2,
    # Size of the quiet-mode window (the guest output is scaled to it).
    [int]$WindowWidth = 960,
    [int]$WindowHeight = 540,
    [string[]]$GameArgs = @(),
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $repo 'out\build\relwithdebinfo\ProvingGround.exe'
$tracy = Join-Path $repo 'tools\windows-0.13.1'
$routeDir = Join-Path $repo ".local\routes\$Route"
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build RelWithDebInfo first: build.ps1 -Configuration RelWithDebInfo' }
if (-not $NoTracy -and -not (Test-Path -LiteralPath "$tracy\tracy-capture.exe")) { throw "Tracy 0.13.1 tools are missing from $tracy" }
if (-not (Test-Path -LiteralPath "$routeDir\route.txt")) { throw "Route not found: $routeDir\route.txt (record it with tools\dev\route.ps1 record $Route)" }
if (-not (Select-String -LiteralPath "$routeDir\route.txt" -Pattern ' measure_stop' -Quiet)) {
    throw 'The route has no measure_stop; the game would never close by itself.'
}
$sdkSource = Join-Path $repo 'thirdparty\rexglue-sdk'
$toolchain = Join-Path $repo '.local\toolchain.json'
if (Test-Path -LiteralPath $toolchain) {
    $settings = Get-Content -LiteralPath $toolchain -Raw | ConvertFrom-Json
    if ($settings.sdkSource) { $sdkSource = $settings.sdkSource }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$suffix = if ($Label) { "-$Label" } else { '' }
$out = Join-Path $repo "out\traces\$Route-$stamp$suffix"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# Same starting save every time, never the real one.
$replayUser = Join-Path $repo "out\replay-user\$Route"
robocopy "$routeDir\start-save" $replayUser /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Could not copy the route starting save' }

$gameArgs = @("--input_replay=$routeDir\route.txt",
    '--input_replay_quit_on_stop', "--perf_log_csv=$out\frames.csv",
    "--game_data_root=$repo\assets", "--user_data_root=$replayUser",
    "--cache_root=$repo\.local\cache-vulkan", "--log_file=$out\game.log") + $GameArgs
if ($NoVsync) { $gameArgs += '--frame_rate_limit=0' }
if (-not $NoTracy) { $gameArgs += '--tracy' }
if ($Sample) { $gameArgs += "--sample_profile=$out\samples.txt" }
if ($Timeline) { $gameArgs += "--timeline_trace=$out\timeline.csv" }
if (-not $Visible) {
    $gameArgs += @('--audio_mute', '--fullscreen=false', "--window_width=$WindowWidth",
        "--window_height=$WindowHeight", "--monitor=$Monitor")
    # SDL reads hints from the environment: show the window without activating it.
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
}

# What this run measured, so an earlier run with the same build and settings can be
# reused instead of measuring again.
$build = Get-Item -LiteralPath $exe
$sdkHead = (git -C $sdkSource rev-parse --short HEAD 2>$null)
$sdkDiff = (git -C $sdkSource diff HEAD 2>$null | Out-String)
$sdkDiffHash = if ($sdkDiff) {
    $sha = [Security.Cryptography.SHA1]::Create()
    ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($sdkDiff))) -replace '-', '').Substring(0, 10)
} else { 'none' }
$runSettings = [ordered]@{
    route = $Route
    label = $Label
    build = '{0:yyyy-MM-dd HH:mm:ss} ({1} bytes)' -f $build.LastWriteTime, $build.Length
    sdk = "$sdkHead, local changes $sdkDiffHash"
    vsync = if ($NoVsync) { 'off' } else { 'on' }
    window = if ($Visible) { 'fullscreen' } else { "${WindowWidth}x${WindowHeight}" }
    args = ($GameArgs -join ' ')
}
$runSettings.GetEnumerator() | ForEach-Object { '{0}: {1}' -f $_.Key, $_.Value } |
    Set-Content -LiteralPath "$out\settings.txt" -Encoding utf8

Write-Host "Profiling route '$Route' -> $out"
$capture = $null
if (-not $NoTracy) {
    # The capture tool retries until the game opens Tracy's port, so start it first.
    $capture = Start-Process -FilePath "$tracy\tracy-capture.exe" -PassThru -NoNewWindow `
        -ArgumentList @('-a', '127.0.0.1', '-o', "$out\trace.tracy", '-f') `
        -RedirectStandardOutput "$out\capture.txt"
}
$game = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $repo -PassThru
if (-not $game.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $game.Id -Force
    Write-Warning "The game did not reach measure_stop within $TimeoutSeconds s and was closed."
}
if ($capture -and -not $capture.WaitForExit(120000)) { Stop-Process -Id $capture.Id -Force }

$report = Join-Path $out 'report.txt'
Get-Content -LiteralPath "$out\settings.txt" | Tee-Object -FilePath $report
& python (Join-Path $PSScriptRoot 'summarize_fps.py') "$out\frames.csv" | Tee-Object -FilePath $report -Append
# One line per run in out\traces\runs.tsv: search it before measuring a configuration again.
$fps = (Select-String -LiteralPath $report -Pattern 'Effective mean FPS: *([0-9.]+)').Matches |
    Select-Object -First 1 | ForEach-Object { $_.Groups[1].Value }
$heavy = (Select-String -LiteralPath $report -Pattern '1500\+ draws:.*\(max +([0-9]+) fps\)').Matches |
    Select-Object -First 1 | ForEach-Object { $_.Groups[1].Value }
$index = Join-Path $repo 'out\traces\runs.tsv'
if (-not (Test-Path -LiteralPath $index)) {
    "run`tfps`theavy_max_fps`tbuild`tsdk`tvsync`twindow`targs" | Set-Content -LiteralPath $index -Encoding utf8
}
"$(Split-Path -Leaf $out)`t$fps`t$heavy`t$($runSettings.build)`t$($runSettings.sdk)`t$($runSettings.vsync)`t$($runSettings.window)`t$($runSettings.args)" |
    Add-Content -LiteralPath $index -Encoding utf8
if (-not $NoTracy) {
    & python (Join-Path $PSScriptRoot 'analyze_trace.py') "$out\trace.tracy" --sdk-src $sdkSource |
        Tee-Object -FilePath $report -Append
}
if ($Sample) { Write-Host "Samples: $out\samples.txt" }
Write-Host "Report: $report"
