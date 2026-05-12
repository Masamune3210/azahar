# Build script for Azahar
# Uses ninja + MSVC (VS 2026 Community) + VS-bundled cmake

$SourceDir  = "$PSScriptRoot"
$BuildDir   = "$PSScriptRoot\build"
$VSPath     = "D:\Program Files\Microsoft Visual Studio\18\Community"
$MSVCBin    = "$VSPath\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64"
$NinjaPath  = "C:\Strawberry\c\bin\ninja.exe"
$CcachePath = "C:\Strawberry\c\bin\ccache.exe"
$CMakePath  = "$VSPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Qt6Dir     = "$BuildDir\externals\qt\6.9.3\msvc2022_64\lib\cmake\Qt6"

foreach ($tool in @($NinjaPath, $CMakePath)) {
    if (-not (Test-Path $tool)) { Write-Error "Tool not found: $tool"; exit 1 }
}

# Import MSVC environment via VsDevCmd
$VsDevCmd = "$VSPath\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $VsDevCmd)) { Write-Error "VsDevCmd.bat not found at: $VsDevCmd"; exit 1 }
$envVars = & "C:\Windows\System32\cmd.exe" /c "`"$VsDevCmd`" -arch=x64 -no_logo && set" 2>&1
foreach ($line in $envVars) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

# Delete stale CMakeCache.txt if it points to a different source directory
$cacheFile = "$BuildDir\CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cachedSource = Select-String -Path $cacheFile -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)" |
                    ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
    if ($cachedSource -and ($cachedSource -ne $SourceDir.Replace('\','/'))) {
        Write-Host "Stale CMakeCache.txt detected (was: $cachedSource), removing..." -ForegroundColor Yellow
        Remove-Item $cacheFile -Force
    }
}

# Configure cmake
Write-Host "Configuring cmake..." -ForegroundColor Cyan
$cmakeArgs = @(
    "-S", $SourceDir,
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_MAKE_PROGRAM=$NinjaPath",
    "-DCMAKE_C_COMPILER=$MSVCBin\cl.exe",
    "-DCMAKE_CXX_COMPILER=$MSVCBin\cl.exe",
    "-DQt6_DIR=$Qt6Dir"
)
if (Test-Path $CcachePath) {
    $cmakeArgs += "-DCMAKE_C_COMPILER_LAUNCHER=$CcachePath"
    $cmakeArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$CcachePath"
}
& $CMakePath @cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed"; exit $LASTEXITCODE }

# Build
$jobs = $args[0]
if (-not $jobs) { $jobs = [Environment]::ProcessorCount }

Write-Host "Building Azahar with $jobs parallel jobs..." -ForegroundColor Cyan
Push-Location $BuildDir
try {
    & $NinjaPath -j $jobs citra_meta
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build FAILED (exit code $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "Build succeeded." -ForegroundColor Green
} finally {
    Pop-Location
}
