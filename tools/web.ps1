# Builds the web test harness (sim/web) and opens it in your browser.
#   powershell -File tools/web.ps1            build + open
#   powershell -File tools/web.ps1 -NoOpen    build only
#   powershell -File tools/web.ps1 -Size 360  360 px screen (Waveshare knob)
# Needs the Emscripten SDK in %USERPROFILE%\emsdk (see sim/web/README.md) and GNU make or ninja.
param([switch]$NoOpen, [int]$Size = 480)
# No $ErrorActionPreference = 'Stop': Windows PowerShell 5 turns any stderr text from native tools
# (emsdk's banner, cmake notes) into a terminating error. Exit codes are checked instead.
$root = Split-Path -Parent $PSScriptRoot
$emsdk = Join-Path $env:USERPROFILE 'emsdk\emsdk_env.ps1'
if (-not (Test-Path $emsdk)) { throw "Emscripten SDK not found at $emsdk. See sim/web/README.md." }
$env:EMSDK_QUIET = '1'
& $emsdk *> $null
if (-not (Get-Command emcc -ErrorAction SilentlyContinue)) { throw 'emsdk_env.ps1 did not put emcc on PATH' }

$build = Join-Path $root "sim\build-web-$Size"
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    $gen = if (Get-Command ninja -ErrorAction SilentlyContinue) { @('-G', 'Ninja') }
           else { @('-G', 'MinGW Makefiles', '-DCMAKE_MAKE_PROGRAM=make') }
    $extra = @()
    $lv = Join-Path $root 'build\_deps\lvgl-src'
    if (Test-Path (Join-Path $lv 'CMakeLists.txt')) { $extra += "-DFETCHCONTENT_SOURCE_DIR_LVGL=$($lv -replace '\\','/')" }
    emcmake cmake -S (Join-Path $root 'sim') -B $build @gen -DCMAKE_BUILD_TYPE=Release "-DBREW_DISP_SIZE=$Size" @extra
    if ($LASTEXITCODE) { throw 'configure failed' }
}
cmake --build $build -j 8
if ($LASTEXITCODE) { throw 'build failed' }

$page = Join-Path $build 'web\index.html'
Write-Host "Harness: $page"
if (-not $NoOpen) { Start-Process $page }
