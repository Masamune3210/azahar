# Build script for Azahar
# Uses ninja + VS 2022 Build Tools + VS-bundled cmake

$SourceDir  = "$PSScriptRoot"
$BuildDir   = "$PSScriptRoot\build"
$VSPath     = "D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$MSVCToolsetVersion = "14.44.35207"
$MSVCToolsetPath = "$VSPath\VC\Tools\MSVC\$MSVCToolsetVersion"
$MSVCBin    = "$MSVCToolsetPath\bin\Hostx64\x64"
$NinjaPath  = "C:\Strawberry\c\bin\ninja.exe"
$CcachePath = "C:\Strawberry\c\bin\ccache.exe"
$CMakePath  = "$VSPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Qt6Dir     = "$BuildDir\externals\qt\6.9.3\msvc2022_64\lib\cmake\Qt6"

foreach ($tool in @($NinjaPath, $CMakePath, "$MSVCBin\cl.exe", "$MSVCBin\link.exe", "$MSVCBin\lib.exe", "$MSVCBin\ml64.exe")) {
    if (-not (Test-Path $tool)) { Write-Error "Tool not found: $tool"; exit 1 }
}

# Remove stale Visual Studio state inherited from the caller. The compiler and
# STL include/lib paths must come from the same MSVC toolset.
foreach ($name in @(
    "INCLUDE",
    "LIB",
    "LIBPATH",
    "VCINSTALLDIR",
    "VCToolsInstallDir",
    "VCToolsVersion",
    "VisualStudioVersion",
    "VSINSTALLDIR"
)) {
    [System.Environment]::SetEnvironmentVariable($name, $null, "Process")
}

# Import MSVC environment via VsDevCmd
$VsDevCmd = "$VSPath\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $VsDevCmd)) { Write-Error "VsDevCmd.bat not found at: $VsDevCmd"; exit 1 }
$envVars = & "C:\Windows\System32\cmd.exe" /c "`"$VsDevCmd`" -arch=x64 -vcvars_ver=$MSVCToolsetVersion -no_logo && set" 2>&1
foreach ($line in $envVars) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

$staleVsEntries = @("D:\Program Files\Microsoft Visual Studio\18\Community", "14.50.35717", "14.51.36231")
foreach ($name in @("Path", "INCLUDE", "LIB", "LIBPATH")) {
    $value = [System.Environment]::GetEnvironmentVariable($name, "Process")
    if ($value) {
        $filtered = foreach ($entry in ($value -split ";")) {
            if ([string]::IsNullOrWhiteSpace($entry)) {
                continue
            }

            $isStale = $false
            foreach ($staleEntry in $staleVsEntries) {
                if ($entry.IndexOf($staleEntry, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    $isStale = $true
                    break
                }
            }

            if (-not $isStale) {
                $entry
            }
        }
        [System.Environment]::SetEnvironmentVariable($name, ($filtered -join ";"), "Process")
        $value = [System.Environment]::GetEnvironmentVariable($name, "Process")
    }

    foreach ($staleEntry in $staleVsEntries) {
        if ($value -and $value.IndexOf($staleEntry, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Write-Error "$name still contains stale Visual Studio entry: $staleEntry"
            exit 1
        }
    }
}

$CLPath = "$MSVCBin\cl.exe"
$LinkPath = "$MSVCBin\link.exe"
$LibPath = "$MSVCBin\lib.exe"
$ML64Path = "$MSVCBin\ml64.exe"
$WindowsSdkVersion = [System.Environment]::GetEnvironmentVariable("WindowsSDKLibVersion", "Process")
if (-not $WindowsSdkVersion) { $WindowsSdkVersion = "10.0.26100.0\" }
$WindowsSdkVersion = $WindowsSdkVersion.TrimEnd("\")
$WindowsSdkRoot = [System.Environment]::GetEnvironmentVariable("WindowsSdkDir", "Process")
if (-not $WindowsSdkRoot) { $WindowsSdkRoot = "C:\Program Files (x86)\Windows Kits\10\" }
$WindowsSdkLibRoot = Join-Path $WindowsSdkRoot "lib\$WindowsSdkVersion"
$NetFxSdkLib = "C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\lib\um\x64"

$explicitLibPaths = @(
    "$MSVCToolsetPath\lib\x64",
    "$MSVCToolsetPath\ATLMFC\lib\x64",
    "$WindowsSdkLibRoot\ucrt\x64",
    "$WindowsSdkLibRoot\um\x64",
    $NetFxSdkLib
) | Where-Object { Test-Path $_ }

if (-not ($explicitLibPaths | Where-Object { Test-Path (Join-Path $_ "kernel32.lib") })) {
    Write-Error "kernel32.lib was not found in the resolved Windows SDK library paths."
    exit 1
}

$LinkerFlags = ($explicitLibPaths | ForEach-Object { "/LIBPATH:`"$_`"" }) -join " "

# Delete stale CMake state if it points to a different source directory or compiler toolset.
$cacheFile = "$BuildDir\CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cachedSource = Select-String -Path $cacheFile -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)" |
                    ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
    $expectedCompiler = $CLPath.Replace('\','/')
    $cacheText = Get-Content -LiteralPath $cacheFile -Raw
    if (($cachedSource -and ($cachedSource -ne $SourceDir.Replace('\','/'))) -or
        ($cacheText -like "*Microsoft Visual Studio*" -and $cacheText -notlike "*$expectedCompiler*") -or
        ($cacheText -like "*14.50.35717*") -or
        ($cacheText -like "*14.51.36231*") -or
        ($cacheText -like "*Microsoft Visual Studio/18*")) {
        Write-Host "Stale CMake state detected, removing CMakeCache.txt and CMakeFiles..." -ForegroundColor Yellow
        Remove-Item $cacheFile -Force
        Remove-Item "$BuildDir\CMakeFiles" -Recurse -Force -ErrorAction SilentlyContinue
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
    "-DCMAKE_C_COMPILER=$CLPath",
    "-DCMAKE_CXX_COMPILER=$CLPath",
    "-DCMAKE_ASM_COMPILER=$CLPath",
    "-DCMAKE_ASM_MASM_COMPILER=$ML64Path",
    "-DCMAKE_LINKER=$LinkPath",
    "-DCMAKE_AR=$LibPath",
    "-DCMAKE_EXE_LINKER_FLAGS=$LinkerFlags",
    "-DCMAKE_SHARED_LINKER_FLAGS=$LinkerFlags",
    "-DCMAKE_MODULE_LINKER_FLAGS=$LinkerFlags",
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

# Only reached if build succeeded (failures call exit above)
$ReleaseDir = "$BuildDir\bin\Release"
$DeployDir  = "F:\Emulation\Emulators\Azahar"
Write-Host "Deploying $ReleaseDir -> $DeployDir..." -ForegroundColor Cyan
$deployFailed = 0
foreach ($item in Get-ChildItem $ReleaseDir) {
    try {
        Copy-Item $item.FullName -Destination $DeployDir -Force -ErrorAction Stop
        Remove-Item $item.FullName -Recurse -Force -ErrorAction Stop
    } catch {
        Write-Warning "Failed to deploy $($item.Name): $_"
        $deployFailed++
    }
}
if ($deployFailed -gt 0) {
    Write-Host "Deploy finished with $deployFailed file(s) that could not be replaced (emulator still open?)." -ForegroundColor Yellow
    exit 1
}
Write-Host "Deploy complete." -ForegroundColor Green
