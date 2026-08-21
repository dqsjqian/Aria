# build-msvc.ps1 — Aria framework + tests + packaging on Visual Studio.
#
# All-in-one script: by default builds framework + tests + ctest + a
# release package. AriaTools is the separately maintained flagship sample.
#
# Default toolchain: Visual Studio (MSVC), located via vswhere. Supports
# VS 2022 / 2026 (and any future version vswhere can find). The generator
# string is derived from the detected VS major version + year, so this
# script does NOT hardcode a specific VS version.
#
# Usage:
#   build-msvc.ps1                    # default: Release framework + tests + ctest + package
#   build-msvc.ps1 debug              # Debug framework + tests (no package)
#   build-msvc.ps1 tests              # Release framework + tests + ctest (no package)
#   build-msvc.ps1 asan               # Debug + /fsanitize=address (MSVC has no UBSan)
#   build-msvc.ps1 pack-zip           # Default flow plus a .zip archive
#   build-msvc.ps1 clean              # Wipe build/
#
# Environment variables:
#   $env:QT_DIR="D:\worksoft\Qt\6.11.1\msvc2022_64"
#   $env:ARIA_NO_QT6="1"
#   $env:ARIA_VS_GENERATOR="Visual Studio 18 2026"  # override generator
#
# AriaTools is the separately maintained flagship sample application.

param(
    [string]$Mode = "default"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$OriginalDir = Get-Location

try {
    # Remove MSYS2/GCC environment overrides that confuse MSVC
    foreach ($e in @("INCLUDE", "LIB", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH")) {
        $val = [Environment]::GetEnvironmentVariable($e)
        if ($val -and $val -match "msys64|mingw") {
            [Environment]::SetEnvironmentVariable($e, $null)
        }
    }

    # ── 修复 MSBuild v180 (VS 2026) 的 Path/PATH 大小写冲突 bug ──────────
    # .NET 的 ProcessStartInfo.EnvironmentVariables 是区分大小写的
    # StringDictionary。如果进程环境块里同时有 "Path"/"PATH"/"path" 等
    # 多个大小写变体（某些父进程如 WorkBuddy 会创建多个），MSBuild 调用
    # CL.exe 时会抛 ArgumentException "已添加项"，导致 CMake 的编译器
    # 识别失败 ("The CXX compiler identification is unknown")。
    # 解法：逐个删除所有 PATH 变体，只保留一个统一的大写 "PATH"。
    $pathVariants = [System.Environment]::GetEnvironmentVariables().Keys | Where-Object { $_ -ieq "path" }
    if ($pathVariants.Count -gt 1) {
        $pathValue = [Environment]::GetEnvironmentVariable("PATH")
        foreach ($v in $pathVariants) {
            [Environment]::SetEnvironmentVariable($v, $null)
        }
        [Environment]::SetEnvironmentVariable("PATH", $pathValue)
    }

    # Auto-detect Visual Studio and set up MSVC environment.
    # vcvars64.bat cannot be sourced in this context; manually populate
    # the variables CMake needs to find the MSVC toolchain.
    #
    # Supports any VS version vswhere can find (2022, 2026, ...). The
    # generator string is derived from the VS major version + year, NOT
    # hardcoded — so this script does not break when a new VS ships.
    $vsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        # Fallback: vswhere sometimes lives under Common7
        $vsWhere2 = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vsWhere2) { $vsWhere = $vsWhere2 }
    }

    $vsPath = $null
    $vsMajor = $null
    $vsYear = $null
    if (Test-Path $vsWhere) {
        # Prefer a VS that actually has the VC tools component.
        $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if (-not $vsPath) {
            # No VC-tools VS found; fall back to any VS install.
            $vsPath = & $vsWhere -latest -products * -property installationPath 2>$null
        }
        if ($vsPath) {
            $vsVer = & $vsWhere -latest -products * -property installationVersion 2>$null
            if ($vsVer -match '^(\d+)') { $vsMajor = $matches[1] }
            $vsName = & $vsWhere -latest -products * -property displayName 2>$null
            if ($vsName -match '(\d{4})\s*$') { $vsYear = $matches[1] }
        }
    }

    if (-not $vsPath) {
        # Fallback to known install locations (covers cases where vswhere
        # is missing or the install isn't registered).
        $fallbackRoots = @(
            "D:\worksoft\VS2026", "D:\worksoft\VS2022",
            "C:\Program Files\Microsoft Visual Studio\2026\Professional",
            "C:\Program Files\Microsoft Visual Studio\2026\Enterprise",
            "C:\Program Files\Microsoft Visual Studio\2026\Community",
            "C:\Program Files\Microsoft Visual Studio\2022\Professional",
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
            "C:\Program Files\Microsoft Visual Studio\2022\Community"
        )
        foreach ($p in $fallbackRoots) {
            if (Test-Path (Join-Path $p "VC\Auxiliary\Build\vcvars64.bat")) {
                $vsPath = $p
                if ($p -match 'VS(20\d{2})') { $vsYear = $matches[1]; $vsMajor = if ($vsYear -eq '2026') { 18 } elseif ($vsYear -eq '2022') { 17 } else { 17 } }
                break
            }
        }
    }

    if ($vsPath) {
        $vsLabel = if ($vsYear) { " ($vsYear)" } else { "" }
        Write-Host "[msvc] VS at $vsPath$vsLabel"

        # Locate the MSVC toolchain version (newest under VC\Tools\MSVC\).
        # Use -ErrorAction SilentlyContinue: under Stop-mode, a missing
        # path would otherwise terminate the whole script.
        $msvcDirs = Get-ChildItem "$vsPath\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if (-not $msvcDirs) {
            Write-Error "VS found at $vsPath but VC\Tools\MSVC is empty. Install the 'MSVC v143/v144 - VS 2022/2026 C++ x64/x86 build tools' component."
            exit 1
        }
        $msvc = $msvcDirs | Select-Object -First 1

        # Windows Kits: read from registry (HKLM\...\Windows Kits\Installed Roots\KitsRoot10).
        # Do NOT hardcode "C:\Program Files (x86)\Windows Kits\10" — on machines
        # where the SDK is installed to D:\Windows Kits\10 the hardcoded path
        # silently points at a nonexistent dir and breaks UCRT include/lib resolution.
        $kitsRoot = $null
        try {
            $reg = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" -Name KitsRoot10 -ErrorAction Stop
            if ($reg.KitsRoot10) { $kitsRoot = $reg.KitsRoot10.TrimEnd('\') }
        } catch { }
        if (-not $kitsRoot) {
            foreach ($cand in @("C:\Program Files (x86)\Windows Kits\10", "D:\Windows Kits\10", "C:\Windows Kits\10")) {
                if (Test-Path $cand) { $kitsRoot = $cand; break }
            }
        }
        if (-not $kitsRoot) {
            Write-Error "Windows Kits not found. Install the Windows 10/11 SDK."
            exit 1
        }
        # SDK version dirs live under Include/ (e.g. Include\10.0.28000.0),
        # NOT under the Windows Kits\10 root. Listing the root only gives
        # Include/, Lib/, bin/ — none match ^\d.
        $kitsDirs = Get-ChildItem "$kitsRoot\Include" -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d' } | Sort-Object Name -Descending
        if (-not $kitsDirs) {
            Write-Error "Windows Kits at $kitsRoot has no SDK version dirs under Include/. Install the Windows 10/11 SDK."
            exit 1
        }
        $kitsVer = ($kitsDirs | Select-Object -First 1).Name
        $clDir = "$vsPath\VC\Tools\MSVC\$($msvc.Name)\bin\Hostx64\x64"

        $env:PATH    = "$clDir;$vsPath\Common7\IDE;$vsPath\MSBuild\Current\Bin;$env:PATH"
        $env:INCLUDE = "$($msvc.FullName)\include;$kitsRoot\Include\$kitsVer\ucrt;$kitsRoot\Include\$kitsVer\um;$kitsRoot\Include\$kitsVer\shared"
        $env:LIB     = "$($msvc.FullName)\lib\x64;$kitsRoot\Lib\$kitsVer\ucrt\x64;$kitsRoot\Lib\$kitsVer\um\x64"

        Write-Host "[msvc] MSVC tools : $($msvc.Name)"
        Write-Host "[msvc] Windows SDK: $kitsVer ($kitsRoot)"
    } else {
        Write-Warning "Visual Studio not found; CMake may fail to find the MSVC compiler"
    }

    # Derive the CMake generator from the detected VS version.
    # e.g. VS 2026 → "Visual Studio 18 2026", VS 2022 → "Visual Studio 17 2022".
    # Allow $env:ARIA_VS_GENERATOR to override (for exotic setups).
    if ($env:ARIA_VS_GENERATOR) {
        $Generator = $env:ARIA_VS_GENERATOR
    } elseif ($vsMajor -and $vsYear) {
        $Generator = "Visual Studio $vsMajor $vsYear"
    } else {
        # Last-resort fallback. VS 2022 is the most commonly installed.
        $Generator = "Visual Studio 17 2022"
        Write-Warning "Could not determine VS version; defaulting to '$Generator'. Set `$env:ARIA_VS_GENERATOR to override."
    }

    Set-Location $RepoRoot

    $BuildDir = "build/flavors/msvc"
    $Jobs = if ($env:NUMBER_OF_PROCESSORS) { $env:NUMBER_OF_PROCESSORS } else { 4 }

# ── Mode → CMake options ─────────────────────────────────────────────────────
$DoCTest   = $false
$DoPackage = $false
$DoArchive = $false

switch ($Mode) {
    "clean" {
        Write-Host "wiping $BuildDir/"
        if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
        exit 0
    }
    { $_ -in "default", "release" } {
        $CMakeOpts = @("-DARIA_BUILD_TESTS=ON")
        $BuildConfig = "Release"
        $DoCTest   = $true
        $DoPackage = $true
    }
    "debug" {
        $CMakeOpts = @("-DARIA_BUILD_TESTS=ON")
        $BuildConfig = "Debug"
    }
    "tests" {
        $CMakeOpts = @("-DARIA_BUILD_TESTS=ON")
        $BuildConfig = "Release"
        $DoCTest = $true
    }
    "asan" {
        # MSVC's ASan requires the Debug configuration; UBSan/TSan are
        # not provided on MSVC. The cmake side reads ARIA_ENABLE_ASAN
        # and emits /fsanitize=address.
        $CMakeOpts = @("-DARIA_BUILD_TESTS=ON", "-DARIA_ENABLE_ASAN=ON")
        $BuildConfig = "Debug"
        $DoCTest = $true
    }
    { $_ -in "pack-zip", "pack", "zip" } {
        $CMakeOpts = @("-DARIA_BUILD_TESTS=ON")
        $BuildConfig = "Release"
        $DoCTest   = $true
        $DoPackage = $true
        $DoArchive = $true
    }
    default {
        Write-Host "unknown mode: $Mode"
        Write-Host "valid: (none) | release | debug | tests | asan | pack-zip | clean"
        exit 1
    }
}

# ── CMake ─────────────────────────────────────────────────────────────────────
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) { $cmake = Get-Command cmake -ErrorAction SilentlyContinue }
if (-not $cmake) { Write-Error "cmake not found. https://cmake.org/download/"; exit 1 }
$cmakePath = $cmake.Source

# ── Qt6 (auto-detect, framework adapter + tests need it) ─────────────────────
function Find-Qt6 {
    if ($env:ARIA_NO_QT6 -eq "1") { return $null }
    if ($env:QT_DIR) {
        if (Test-Path (Join-Path $env:QT_DIR "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $env:QT_DIR
        }
    }
    $roots = @("D:\worksoft\Qt", "C:\Qt", "D:\Qt")
    $kitOrder = @("msvc2022_64", "msvc2019_64", "mingw_64")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $versions = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^6\.' } | Sort-Object Name -Descending
        foreach ($v in $versions) {
            foreach ($kit in $kitOrder) {
                $p = Join-Path $v.FullName $kit
                if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
                    return $p
                }
            }
        }
    }
    return $null
}

$Qt6Dir = Find-Qt6
if ($Qt6Dir) {
    Write-Host "Qt6 detected at $Qt6Dir -- adapter enabled"
    $CMakeOpts += @("-DARIA_BUILD_QT6=ON", "-DCMAKE_PREFIX_PATH=$Qt6Dir")
}

# ── Configure ────────────────────────────────────────────────────────────────
Write-Host "configuring ($Mode)..."
$cfgArgs = @("-S", $RepoRoot, "-B", $BuildDir, "-G", $Generator) + $CMakeOpts
& $cmakePath @cfgArgs
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit $LASTEXITCODE }

# ── Build ────────────────────────────────────────────────────────────────────
Write-Host "building ($BuildConfig)..."
# MSBuild v18 (VS 2026) has a child-node timeout bug (MSB4166) that crashes
# multi-process builds. Disable node reuse and force /m:1 (single process)
# to work around it. The `--` passes flags through to MSBuild.
# This is slower than multi-process but is the only reliable workaround
# until MSBuild v18 fixes the bug.
$env:MSBUILDDISABLENODEREUSE = "1"
& $cmakePath --build $BuildDir --config $BuildConfig -- /m:1
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit $LASTEXITCODE }

# ── Test ─────────────────────────────────────────────────────────────────────
if ($DoCTest) {
    Write-Host "running ctest..."
    # DLL search paths: unified bin/ output + Qt bin + VC runtime
    $testPath = @(
        (Join-Path $BuildDir "bin\$BuildConfig")
    )
    if ($Qt6Dir) { $testPath += (Join-Path $Qt6Dir "bin") }
    # VC++ runtime (vcruntime140.dll etc.) — use the VS path we detected
    # at the top of the script, NOT a hardcoded "2022" glob (which misses
    # VS 2026 installs on machines without 2022).
    if ($vsPath) {
        $msvcDirs = Get-ChildItem "$vsPath\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($msvcDirs) { $testPath += (Join-Path $msvcDirs[0].FullName "bin\Hostx64\x64") }
    }
    $env:PATH = ($testPath -join ";") + ";" + $env:PATH

    # Use the ctest that ships next to the cmake we found — MSYS2's ctest
    # does POSIX path translation that mangles Windows DLL search paths.
    $ctestPath = Join-Path (Split-Path -Parent $cmakePath) "ctest.exe"
    if (-not (Test-Path $ctestPath)) {
        $ctestCmd = Get-Command ctest -ErrorAction SilentlyContinue
        if ($ctestCmd) { $ctestPath = $ctestCmd.Source }
    }
    if (-not (Test-Path $ctestPath)) {
        Write-Error "ctest not found next to cmake ($cmakePath) and not on PATH"
        exit 1
    }
    $proc = Start-Process $ctestPath -ArgumentList @("--test-dir", $BuildDir, "-C", $BuildConfig, "--output-on-failure") -NoNewWindow -Wait -PassThru
    if ($proc.ExitCode -ne 0) { Write-Error "Tests failed"; exit $proc.ExitCode }
}

# ── Package ──────────────────────────────────────────────────────────────────
if ($DoPackage) {
    Write-Host "packaging release..."
    & $cmakePath --build $BuildDir --target package-release --config $BuildConfig -- /m:1
    if ($LASTEXITCODE -ne 0) { Write-Error "Package failed"; exit $LASTEXITCODE }
}

# ── Optional archive ─────────────────────────────────────────────────────────
if ($DoArchive) {
    Write-Host "creating archive..."
    & $cmakePath --build $BuildDir --target package-archive --config $BuildConfig -- /m:1
    if ($LASTEXITCODE -ne 0) { Write-Error "Archive failed"; exit $LASTEXITCODE }
    Write-Host ""
    Write-Host "Archive created:"
    Get-ChildItem "build\dist\archives\*.*" -ErrorAction SilentlyContinue | ForEach-Object { "  $_" }
}

Write-Host ""
Write-Host "$Mode done."
Write-Host "  Solution : $RepoRoot\$BuildDir\aria.sln"
if ($DoPackage) { Write-Host "  Package  : $RepoRoot\$BuildDir\release\" }

} finally {
    Set-Location $OriginalDir
}
