# Repeatable controller routes for performance testing (see docs/DEVELOPMENT.md).
#
#   route.ps1 record  <name>   Snapshot your save, then record your controller input from boot.
#                              F9 marks the start of the measurement window, and again its end.
#   route.ps1 replay  <name>   Replay the route from the snapshot; afterwards you take over.
#   route.ps1 measure <name>   Replay, close the game at measure_stop and summarize the FPS.
#
# Extra arguments after the name go to the game, e.g. route.ps1 measure park --render_resolution=1440p
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('record', 'replay', 'measure')][string]$Action,
    [Parameter(Mandatory, Position = 1)][string]$Name,
    # Save folder to snapshot when recording (default: the game's normal save folder).
    [string]$UserData = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'ProvingGroundRecomp'),
    [Parameter(ValueFromRemainingArguments)][string[]]$GameArgs = @()
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $repo 'out\build\relwithdebinfo\ProvingGround.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build RelWithDebInfo first: build.ps1 -Configuration RelWithDebInfo' }
$route = Join-Path $repo ".local\routes\$Name"
$measurements = Join-Path $repo 'out\measurements'
New-Item -ItemType Directory -Force -Path $measurements | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
# The game is a GUI program: start it and wait, quoting each argument.
function Start-Game([string[]]$Arguments) {
    $quoted = $Arguments | ForEach-Object { '"' + $_ + '"' }
    Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $repo -Wait
}
$common = @("--game_data_root=$repo\assets", "--cache_root=$repo\.local\cache-vulkan")

if ($Action -eq 'record') {
    if (Test-Path -LiteralPath "$route\route.txt") { throw "$route\route.txt exists; delete that folder to record again." }
    New-Item -ItemType Directory -Force -Path $route | Out-Null
    # Replays start from this snapshot of the save.
    robocopy $UserData "$route\start-save" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Could not copy $UserData" }
    Write-Host "Recording '$Name'. Play with the controller from boot; F9 marks the measurement window."
    Start-Game ($common + @("--user_data_root=$UserData", "--input_record=$route\route.txt",
        "--perf_log_csv=$measurements\record-$Name-$stamp.csv") + $GameArgs)
    Write-Host "Route saved: $route\route.txt"
    exit 0
}

if (-not (Test-Path -LiteralPath "$route\route.txt")) { throw "Route not found: $route\route.txt" }
$replayUser = Join-Path $repo "out\replay-user\$Name"
robocopy "$route\start-save" $replayUser /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Could not copy the route starting save' }
$csv = Join-Path $measurements "$Action-$Name-$stamp.csv"
$replayArgs = @("--user_data_root=$replayUser", "--input_replay=$route\route.txt", "--perf_log_csv=$csv")
if ($Action -eq 'measure') {
    # Physical controllers are ignored so a touch cannot desynchronize the route.
    $replayArgs += '--input_replay_quit_on_stop'
    Write-Host "Measuring '$Name'; the game closes at measure_stop."
} else {
    Write-Host "Replaying '$Name'; when the route ends, your controller takes over."
}
Start-Game ($common + $replayArgs + $GameArgs)
Write-Host "Measurement: $csv"
if ($Action -eq 'measure') { & python (Join-Path $PSScriptRoot 'summarize_fps.py') $csv }
