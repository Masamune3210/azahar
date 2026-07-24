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

# VsDevCmd expands PATH inside cmd.exe, whose batch parser rejects very long
# input lines. Initialize it with only core Windows paths, then merge the
# resulting Visual Studio paths back into the caller's original PATH.
$originalPath = [System.Environment]::GetEnvironmentVariable("Path", "Process")
$windowsRoot = [System.Environment]::GetEnvironmentVariable("SystemRoot", "Process")
if (-not $windowsRoot) { $windowsRoot = "C:\Windows" }
$vsBootstrapPath = @(
    "$windowsRoot\System32",
    $windowsRoot,
    "$windowsRoot\System32\Wbem",
    "$windowsRoot\System32\WindowsPowerShell\v1.0"
) -join ";"
[System.Environment]::SetEnvironmentVariable("Path", $vsBootstrapPath, "Process")

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
$vsDevCmdExitCode = $LASTEXITCODE
foreach ($line in $envVars) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$vsDevPath = [System.Environment]::GetEnvironmentVariable("Path", "Process")
$seenPathEntries =
    [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$combinedPathEntries = foreach ($entry in (($vsDevPath + ";" + $originalPath) -split ";")) {
    $trimmedEntry = $entry.Trim()
    if ($trimmedEntry -and $seenPathEntries.Add($trimmedEntry)) {
        $trimmedEntry
    }
}
$combinedPath = $combinedPathEntries -join ";"
if ($combinedPath.Length -ge 32767) {
    Write-Error "Combined build PATH is too long for a Windows process environment."
    exit 1
}
[System.Environment]::SetEnvironmentVariable("Path", $combinedPath, "Process")

$resolvedToolsetPath = [System.Environment]::GetEnvironmentVariable("VCToolsInstallDir", "Process")
$toolsetEnvironmentValid =
    $resolvedToolsetPath -and
    [string]::Equals(
        $resolvedToolsetPath.TrimEnd("\"),
        $MSVCToolsetPath.TrimEnd("\"),
        [StringComparison]::OrdinalIgnoreCase
    )
if (-not $toolsetEnvironmentValid) {
    $vsDevCmdDiagnostics = @($envVars | Where-Object { $_ -notmatch '^[^=]+=.*$' }) -join [Environment]::NewLine
    Write-Error "VsDevCmd.bat did not initialize MSVC $MSVCToolsetVersion (exit code $vsDevCmdExitCode). $vsDevCmdDiagnostics"
    exit $(if ($vsDevCmdExitCode -ne 0) { $vsDevCmdExitCode } else { 1 })
}
if ($vsDevCmdExitCode -ne 0) {
    Write-Warning "VsDevCmd.bat returned exit code $vsDevCmdExitCode but initialized the requested MSVC environment; continuing after path validation."
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
$WindowsSdkIncludeRoot = Join-Path $WindowsSdkRoot "include\$WindowsSdkVersion"
$WindowsSdkLibRoot = Join-Path $WindowsSdkRoot "lib\$WindowsSdkVersion"
$NetFxSdkInclude = "C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\include\um"
$NetFxSdkLib = "C:\Program Files (x86)\Windows Kits\NETFXSDK\4.8\lib\um\x64"

$explicitIncludePaths = @(
    "$MSVCToolsetPath\include",
    "$MSVCToolsetPath\ATLMFC\include",
    "$VSPath\VC\Auxiliary\VS\include",
    "$WindowsSdkIncludeRoot\ucrt",
    "$WindowsSdkIncludeRoot\um",
    "$WindowsSdkIncludeRoot\shared",
    "$WindowsSdkIncludeRoot\winrt",
    "$WindowsSdkIncludeRoot\cppwinrt",
    $NetFxSdkInclude
) | Where-Object { Test-Path -LiteralPath $_ }

$requiredHeaders = @(
    "$MSVCToolsetPath\include\string",
    "$WindowsSdkIncludeRoot\ucrt\stdio.h",
    "$WindowsSdkIncludeRoot\um\Windows.h"
)
foreach ($header in $requiredHeaders) {
    if (-not (Test-Path -LiteralPath $header)) {
        Write-Error "Required MSVC/Windows SDK header not found: $header"
        exit 1
    }
}
[System.Environment]::SetEnvironmentVariable("INCLUDE", ($explicitIncludePaths -join ";"), "Process")

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

# Ninja owns normal CMake regeneration. This explicit configure is only a
# bootstrap for a new or deliberately cleared build tree. If the toolchain,
# generator, Qt path, or configure arguments change, remove CMakeCache.txt and
# CMakeFiles before running this script so this bootstrap refreshes them.
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
if (Test-Path -LiteralPath $CcachePath) {
    $cmakeArgs += "-DCMAKE_C_COMPILER_LAUNCHER=$CcachePath"
    $cmakeArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$CcachePath"
} else {
    $cmakeArgs += "-DCMAKE_C_COMPILER_LAUNCHER="
    $cmakeArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER="
}

$cmakeStateFiles = @(
    "$BuildDir\CMakeCache.txt",
    "$BuildDir\build.ninja",
    "$BuildDir\CMakeFiles\rules.ninja"
)
$missingCMakeState = @($cmakeStateFiles | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missingCMakeState.Count -gt 0) {
    Write-Host "CMake build state is incomplete; bootstrapping it once..." -ForegroundColor Cyan
    $bootstrapArgs = @("--fresh") + $cmakeArgs
    & $CMakePath @bootstrapArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit $LASTEXITCODE }
} else {
    Write-Host "Using the existing Ninja-managed CMake build graph." -ForegroundColor DarkGray
}

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
