# Builds Proving Ground Recompiled on Windows.
#
#   .\build.ps1                      Release build in out\build\release, staged in out\dist
#   .\build.ps1 -Configuration RelWithDebInfo   Build with symbols and performance counters
#
# Steps: fetch ReXGlue v0.10.0 into thirdparty\rexglue-sdk and apply patches\rexglue-sdk.patch
# (first run only), build the rexglue code generator, generate code from assets\default.xex,
# then build the game. Requires Visual Studio 2022 (or Build Tools) with the C++ workload and
# its Clang component, or a separate LLVM/Clang passed with -LlvmBin.
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 256)][int]$Jobs = [Environment]::ProcessorCount,
    # An existing, already patched ReXGlue source tree instead of thirdparty\rexglue-sdk.
    [string]$SdkSource = $env:REXSDK_SOURCE,
    # Folder with clang.exe/clang++.exe, if not the one that comes with Visual Studio.
    [string]$LlvmBin = $env:LLVM_BIN,
    # Build AMD FidelityFX into the renderer (downloads the FidelityFX SDK).
    [switch]$FidelityFX,
    # Only generate the code; do not build the game.
    [switch]$CodegenOnly,
    # Skip copying the game and its libraries to out\dist.
    [switch]$NoStage
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$sdkTag = 'v0.10.0'
$sdkUrl = 'https://github.com/rexglue/rexglue-sdk.git'
$patch = Join-Path $repo 'patches\rexglue-sdk.patch'

# Optional per-machine settings (ignored by Git): { "sdkSource": "...", "llvmBin": "..." }
$settingsFile = Join-Path $repo '.local\toolchain.json'
if (Test-Path -LiteralPath $settingsFile) {
    $settings = Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json
    if (-not $SdkSource -and $settings.sdkSource) { $SdkSource = $settings.sdkSource }
    if (-not $LlvmBin -and $settings.llvmBin) { $LlvmBin = $settings.llvmBin }
}

if (-not (Test-Path -LiteralPath (Join-Path $repo 'assets\default.xex'))) {
    throw 'Missing assets\default.xex. Extract your Xbox 360 copy of the game into the assets folder first.'
}

$logs = Join-Path $repo 'out\logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

function Invoke-Step {
    param([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$WorkingDirectory = $repo)
    $log = Join-Path $logs "$Name.log"
    Write-Host "==> $Name" -ForegroundColor Cyan
    $previous = $ErrorActionPreference
    Push-Location $WorkingDirectory
    try {
        # Windows PowerShell treats native stderr as errors; judge by the exit code instead.
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 2>&1 | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding utf8
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
        Pop-Location
    }
    if ($code -ne 0) {
        Get-Content -LiteralPath $log -Tail 40 | Write-Host
        throw "$Name failed (exit $code). Full log: $log"
    }
}

# --- ReXGlue SDK source ---
$git = (Get-Command git -ErrorAction SilentlyContinue).Source
if ($SdkSource) {
    $sdk = (Resolve-Path -LiteralPath $SdkSource).Path
} else {
    $sdk = Join-Path $repo 'thirdparty\rexglue-sdk'
    $marker = Join-Path $sdk '.pg-patch'
    $patchHash = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash
    if (-not (Test-Path -LiteralPath (Join-Path $sdk 'CMakeLists.txt'))) {
        if (-not $git) { throw 'Git is required to fetch the ReXGlue SDK.' }
        Invoke-Step 'sdk-clone' $git @('-c', 'core.symlinks=false', '-c', 'core.longpaths=true', 'clone', '--branch', $sdkTag,
            '--depth', '1', $sdkUrl, $sdk)
        Invoke-Step 'sdk-submodules' $git @('-c', 'core.symlinks=false', '-c', 'core.longpaths=true', 'submodule', 'update',
            '--init', '--recursive', '--depth', '1', '--jobs', '8') $sdk
    }
    if (-not (Test-Path -LiteralPath $marker)) {
        Invoke-Step 'sdk-patch' $git @('apply', '--whitespace=nowarn', $patch) $sdk
        Set-Content -LiteralPath $marker -Value $patchHash -Encoding ascii
    } elseif ((Get-Content -LiteralPath $marker -Raw).Trim() -ne $patchHash) {
        Write-Warning ("patches\rexglue-sdk.patch changed since thirdparty\rexglue-sdk was patched. " +
            'Delete thirdparty\rexglue-sdk to fetch and patch it again.')
    }
}

# --- Compiler environment ---
$env:VSLANG = '1033'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2022 or its Build Tools (C++ workload) are required.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the C++ x64 tools was found.' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
cmd /d /c "`"$devcmd`" -arch=amd64 -host_arch=amd64 -no_logo >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$ErrorActionPreference = $previous

$cmakeRoot = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$paths = @((Join-Path $cmakeRoot 'CMake\bin'), (Join-Path $cmakeRoot 'Ninja'))
if ($LlvmBin) {
    $paths = @((Resolve-Path -LiteralPath $LlvmBin).Path) + $paths
} else {
    foreach ($candidate in @((Join-Path $env:ProgramFiles 'LLVM\bin'), (Join-Path $vs 'VC\Tools\Llvm\x64\bin'))) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'clang++.exe')) { $paths = @($candidate) + $paths; break }
    }
}
$env:PATH = ($paths -join ';') + ';' + $env:PATH
$clang = (Get-Command clang++ -ErrorAction SilentlyContinue).Source
if (-not $clang) { throw 'Clang was not found. Install the "C++ Clang Compiler for Windows" Visual Studio component or pass -LlvmBin.' }
$clangC = (Get-Command clang -ErrorAction Stop).Source
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ninja = (Get-Command ninja -ErrorAction Stop).Source
Write-Host "Compiler: $clang"

# --- Configure, build the code generator, generate, build the game ---
$build = Join-Path $repo ("out\build\{0}" -f $Configuration.ToLowerInvariant())
$llvmTools = Split-Path -Parent $clang
$vulkanArgs = @()
if ($FidelityFX -and -not $env:VULKAN_SDK) {
    # FidelityFX only needs the Vulkan headers, which the SDK carries, but its find_package(Vulkan)
    # also wants a loader library: make an import library from the system vulkan-1.dll.
    $vulkanImport = Join-Path $repo 'out\vulkan-import\vulkan-1.lib'
    if (-not (Test-Path -LiteralPath $vulkanImport)) {
        $def = Join-Path $repo 'out\vulkan-import\vulkan-1.def'
        New-Item -ItemType Directory -Force -Path (Split-Path $vulkanImport) | Out-Null
        $names = & (Join-Path $llvmTools 'llvm-readobj.exe') --coff-exports "$env:SystemRoot\System32\vulkan-1.dll" |
            Select-String '^\s*Name: (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
        Set-Content -LiteralPath $def -Encoding ascii -Value (@('LIBRARY vulkan-1.dll', 'EXPORTS') + $names)
        & (Join-Path $llvmTools 'llvm-dlltool.exe') -m i386:x86-64 -d $def -l $vulkanImport
        if ($LASTEXITCODE -ne 0) { throw 'Could not create the Vulkan import library' }
    }
    $vulkanArgs = @("-DVulkan_INCLUDE_DIR=$sdk\thirdparty\vulkan-headers\include", "-DVulkan_LIBRARY=$vulkanImport")
}
function Configure([string]$Game) {
    $configureArgs = @('-S', $repo, '-B', $build, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_CXX_COMPILER=$clang", "-DCMAKE_C_COMPILER=$clangC",
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DREXSDK_DIR=$sdk", "-DPG_BUILD_GAME=$Game",
        '-DREXGLUE_USE_D3D12=OFF', '-DREXGLUE_USE_VULKAN=ON', '-DREXGLUE_BUILD_TESTS=OFF',
        '-DCMAKE_C_FLAGS=-march=x86-64-v2', '-DCMAKE_CXX_FLAGS=-march=x86-64-v2',
        "-DREXGLUE_ENABLE_FIDELITYFX=$(if ($FidelityFX) { 'ON' } else { 'OFF' })") + $vulkanArgs
    Invoke-Step "configure-$Game" $cmake $configureArgs
}

Configure 'OFF'
Invoke-Step 'build-codegen' $cmake @('--build', $build, '--target', 'rexglue', '--parallel', "$Jobs")
# The SDK writes its tools to its own out\<platform> folder, named with a per-configuration suffix.
$toolName = 'rexglue{0}.exe' -f @{ Release = ''; RelWithDebInfo = 'rd'; Debug = 'd' }[$Configuration]
$rexglue = @($build, (Join-Path $sdk 'out')) | Where-Object { Test-Path -LiteralPath $_ } |
    ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -Filter $toolName } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $rexglue) { throw 'The rexglue code generator was not built.' }
Invoke-Step 'codegen' $rexglue.FullName @('codegen', (Join-Path $repo 'pg_config.toml'))
if ($CodegenOnly) { Write-Host 'Code generated in generated\.'; exit 0 }

Configure 'ON'
Invoke-Step 'build-game' $cmake @('--build', $build, '--target', 'pg', '--parallel', "$Jobs")
$exe = Join-Path $build 'ProvingGround.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build finished but $exe is missing." }

if (-not $NoStage) {
    $dist = Join-Path $repo 'out\dist\ProvingGroundRecomp'
    if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Copy-Item -LiteralPath $exe -Destination $dist
    Get-ChildItem -LiteralPath $build -Filter '*.dll' | Copy-Item -Destination $dist
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $dist
    Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination $dist
    Copy-Item -LiteralPath (Join-Path $repo 'docs\PLAYING.md') -Destination (Join-Path $dist 'README.md')
    Write-Host "Ready to play: $dist\ProvingGround.exe" -ForegroundColor Green
} else {
    Write-Host "Built: $exe" -ForegroundColor Green
}
