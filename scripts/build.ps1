# Unified build & package script for Windows (MSYS2 UCRT64).
#
# Default behaviour (no args): build aria framework + tests + ctest +
# release package -> build\flavors\release\
#
# Default toolchain: MSYS2 UCRT64 (GCC 14+ / Clang 18+) + Ninja.
#
# For MSVC / Visual Studio, use `scripts\build-msvc.ps1` instead — it's
# a dedicated script that auto-locates VS 2022 and scrubs MSYS2 env
# vars before running CMake. This script intentionally stays MSYS2-only
# to keep the path short and the toolchain assumptions tight.
#
# Build-tree layout (since 2026-06-08):
#   build\
#     ├── ide\                  VSCode CMake Tools workspace
#     ├── flavors\              command-line build flavors (this script)
#     │     ├── release\
#     │     ├── debug\
#     │     ├── asan\
#     │     └── tsan-gate\
#     ├── platforms\            cross-compilation targets
#     │     └── android\      scripts\build.ps1 android (NDK cross-build)
#     ├── examples\             per-demo cmake caches
#     └── dist\                 release artefacts (only when packaging)
#           ├── tree\
#           └── archives\
#
# Prerequisites (one-time):
#   1. Install MSYS2:  https://www.msys2.org
#   2. Open the "MSYS2 UCRT64" shell and run:
#        pacman -Syu
#        pacman -S --needed mingw-w64-ucrt-x86_64-toolchain `
#                          mingw-w64-ucrt-x86_64-cmake `
#                          mingw-w64-ucrt-x86_64-ninja `
#                          git
#   3. Add C:\msys64\ucrt64\bin to your PATH (or run this script from a
#      "MSYS2 UCRT64" shell — its PATH is already set up).
#
# Usage (from PowerShell or an MSYS2 shell):
#   scripts\build.ps1                # default: Release framework + tests + ctest + package
#   scripts\build.ps1 debug          # Debug framework + tests
#   scripts\build.ps1 tests          # Release framework + tests + ctest (no package)
#   scripts\build.ps1 asan           # Debug + AddressSanitizer + UBSan
#   scripts\build.ps1 pack-zip       # Default flow plus build\packages\aria-*.zip
#   scripts\build.ps1 android        # Android NDK cross-build (JNI adapter)
#   scripts\build.ps1 clean          # Wipe build\
#
# Override compiler:
#   $env:CC="clang"; $env:CXX="clang++"; scripts\build.ps1
#
# Skip MSYS2 auto-detection (use whatever's on PATH):
#   $env:ARIA_NO_MSYS2="1"; scripts\build.ps1
#
# Notes: this script only builds the aria framework + tests; it does
# NOT build examples. To run a demo use examples\<demo>\scripts\run.ps1
# (or run.sh).

param(
    [string]$Mode = "default"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $Root
try {

# ── Deploy-DllDependencies ───────────────────────────────────────────────────
# Mirrors examples/1-qt-showcase/scripts/run.ps1 Copy-Dependencies so that any
# exe in <build>/bin/ can be launched directly without DLL-not-found dialogs.
#
# Strategy (in order):
#   1. windeployqt  -- for any exe that links Qt6, deploy Qt DLLs + plugins
#                      (platforms/qwindows.dll, imageformats/*, tls/*, ...)
#                      into bin/.  This is the *only* correct way to handle Qt
#                      on Windows: copying Qt6Core.dll alone breaks plugin
#                      discovery.  windeployqt brings the whole plugin tree.
#   2. iterative objdump  -- BFS over (exe + every DLL just copied) to resolve
#                      transitive non-Qt, non-system deps from MSYS2 runtime.
#                      Catches libgcc_s_seh-1 / libstdc++-6 / libwinpthread-1
#                      and any other MinGW runtime DLL without hardcoding.
#
# Qt DLLs are NOT skipped by objdump (windeployqt already put them in bin/ with
# their plugin tree, so the loader finds them next to the exe -- correct).
function Deploy-DllDependencies {
    param(
        [Parameter(Mandatory)] [string] $BinDir,
        [string] $Msys2Bin,
        [string] $Qt6Dir
    )

    if (-not (Test-Path $BinDir)) { return }
    if (-not $Msys2Bin -and -not $Qt6Dir) { return }

    # Locate objdump up front (used both for the Qt-link probe and the
    # iterative transitive scan).
    $objdump = $null
    if ($Msys2Bin) {
        $p = Join-Path $Msys2Bin "objdump.exe"
        if (Test-Path $p) { $objdump = $p }
    }
    if (-not $objdump) {
        $c = Get-Command objdump.exe -ErrorAction SilentlyContinue
        if ($c) { $objdump = $c.Source }
    }

    # Index of DLLs already in bin/ (our own build output + anything we copy).
    $presentInBin = @{}
    Get-ChildItem "$BinDir\*.dll" -ErrorAction SilentlyContinue |
        ForEach-Object { $presentInBin[$_.Name.ToLower()] = $true }

    # ── 1. windeployqt: deploy Qt for every exe that links Qt6 ──────────────
    if ($Qt6Dir -and $objdump) {
        $windeployqt = $null
        foreach ($candidate in @(
            (Join-Path $Msys2Bin "windeployqt.exe"),
            (Join-Path $Qt6Dir "bin\windeployqt.exe")
        )) {
            if ($candidate -and (Test-Path $candidate)) { $windeployqt = $candidate; break }
        }
        if ($windeployqt) {
            # Probe each exe: does its import table mention Qt6*.dll?
            # (Direct or via libaria_qt6.dll -- windeployqt itself recurses,
            #  but only into DLLs that live alongside the exe. So we trigger
            #  it on any exe whose *direct* imports include Qt or aria_qt6.)
            # stderr from windeployqt/objdump is benign (warnings about
            # missing translations etc.); swallow it so $ErrorActionPreference
            #="Stop" doesn't abort the build.
            $qtExes = Get-ChildItem "$BinDir\*.exe" -ErrorAction SilentlyContinue | Where-Object {
                $deps = & $objdump -p $_.FullName 2>$null |
                    Select-String '^\s*DLL Name:\s*(.+)$' |
                    ForEach-Object { $_.Matches[0].Groups[1].Value.Trim().ToLower() }
                ($deps | Where-Object { $_ -match '^qt6?\w*\.dll$' -or $_ -match '^libaria_qt6\.dll$' }).Count -gt 0
            }
            foreach ($exe in $qtExes) {
                Write-Host "> windeployqt: $($exe.Name)"
                # windeployqt writes benign warnings (missing translations,
                # skipped plugins) to stderr.  Under $ErrorActionPreference=
                # "Stop" PS5.1 surfaces native stderr as a terminating
                # RemoteException, so temporarily relax the preference for
                # this one call.
                $prevEAP = $ErrorActionPreference
                $ErrorActionPreference = "Continue"
                try {
                    & $windeployqt --no-translations --no-system-d3d-compiler `
                        --no-opengl-sw --no-quick-import $exe.FullName 2>&1 | Out-Null
                } catch {
                    # Swallow the RemoteException -- it's just windeployqt's
                    # stderr warning surfacing as a terminating error.
                } finally {
                    $ErrorActionPreference = $prevEAP
                }
                # Re-index bin/ -- windeployqt may have added Qt DLLs + plugins.
                Get-ChildItem "$BinDir\*.dll" -ErrorAction SilentlyContinue |
                    ForEach-Object { $presentInBin[$_.Name.ToLower()] = $true }
            }
        } else {
            Write-Warning "windeployqt.exe not found under MSYS2 or Qt6 bin/; Qt plugin tree may be incomplete."
        }

        # windeployqt only deploys the platforms plugin matching the host
        # (qwindows.dll).  Test exes also need qoffscreen.dll for headless
        # runs (QT_QPA_PLATFORM=offscreen) and qminimal.dll as a fallback.
        # Copy the whole platforms/ dir so every test scenario works.
        # MSYS2's Qt6 uses a FHS layout (<prefix>/share/qt6/plugins/),
        # the official Qt installer uses <prefix>/plugins/ -- probe both.
        # This runs even when windeployqt is missing, so test exes always
        # have a working platform plugin available.
        $platSrc = $null
        foreach ($p in @(
            (Join-Path $Qt6Dir "plugins\platforms"),
            (Join-Path $Qt6Dir "share\qt6\plugins\platforms")
        )) {
            if (Test-Path $p) { $platSrc = $p; break }
        }
        if ($platSrc) {
            $platDst = Join-Path $BinDir "platforms"
            New-Item -ItemType Directory -Path $platDst -Force | Out-Null
            Copy-Item "$platSrc\*.dll" $platDst -Force -ErrorAction SilentlyContinue
            $platCount = @(Get-ChildItem "$platDst\*.dll" -ErrorAction SilentlyContinue).Count
            Write-Host "> platforms plugins: $platCount deployed"
        }
    }

    # ── 2. Iterative objdump: transitive non-Qt, non-system deps ───────────
    if (-not $objdump -or -not $Msys2Bin) { return }

    # Windows system DLLs -- always present, never copy.
    $sysRegex = '^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|msvcp\d+|vcruntime\d+|ucrtbase|api-ms-win-|ntdll|mscoree|ws2_32|secur32|crypt32|userenv|rpcrt4|psapi|dbghelp|bcrypt|iphlpapi|setupapi|cfgmgr32|shlwapi|version|winmm|imm32|wininet|urlmon|wtsapi32|authz|clusapi|comdlg32|winspool|ndf|netprofm|nmapi|powrprof|sensapi|userenv|msvcp_win|concrt140|vccorlib140|msvcp140|vcruntime140|vcruntime140_1)\.dll$'

    # BFS work queue: start from every exe + dll in bin/.
    $queue = New-Object System.Collections.Generic.Queue[string]
    foreach ($b in (Get-ChildItem "$BinDir\*.exe","$BinDir\*.dll" -ErrorAction SilentlyContinue)) {
        $queue.Enqueue($b.FullName)
    }

    $copied = 0
    while ($queue.Count -gt 0) {
        $f = $queue.Dequeue()
        $deps = & $objdump -p $f 2>$null |
            Select-String '^\s*DLL Name:\s*(.+)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
        foreach ($d in $deps) {
            $dl = $d.ToLower()
            if ($presentInBin.ContainsKey($dl)) { continue }   # already have it
            if ($dl -match $sysRegex) { continue }             # Windows system DLL
            if ($dl -match '^qt6?\w*\.dll$') { continue }      # Qt -- windeployqt handled it

            # Resolve from MSYS2 bin (MinGW runtime + any other deps).
            $src = Join-Path $Msys2Bin $d
            if (Test-Path $src) {
                Copy-Item $src $BinDir -Force | Out-Null
                Write-Host "  + $d"
                $presentInBin[$dl] = $true
                $copied++
                # Queue the freshly copied DLL so its own deps get scanned too.
                $queue.Enqueue($src)
            }
            # If not found in MSYS2 bin, it's either a system DLL we missed in
            # the regex, or a DLL that should already be on PATH.  Skip silently.
        }
    }
    if ($copied -gt 0) {
        Write-Host "> deployed $copied runtime DLL(s) to $(Split-Path $BinDir -Leaf)/bin/"
    }
}


$BuildDir = switch ($Mode) {
    "default"   { "build/flavors/release" }
    "release"   { "build/flavors/release" }
    "debug"     { "build/flavors/debug" }
    "tests"     { "build/flavors/release" }   # alias: ctest on the release tree
    "asan"      { "build/flavors/asan" }
    "tsan"      { "build/flavors/tsan" }
    "pack-zip"  { "build/flavors/release" }
    "pack"      { "build/flavors/release" }
    "zip"       { "build/flavors/release" }
    "tar"       { "build/flavors/release" }
    "package"   { "build/flavors/release" }
    "tsan-gate" { "build/flavors/tsan-gate" }
    "android"    { "build/platforms/android" }
    "clean"     { "build" }
    default     { "build/flavors/$Mode" }
}
$Jobs = $env:NUMBER_OF_PROCESSORS
if (-not $Jobs) { $Jobs = 4 }

# ── Locate MSYS2 UCRT64 toolchain ────────────────────────────────────────────
function Find-Msys2Bin {
    if ($env:ARIA_NO_MSYS2 -eq "1") { return $null }

    # 1) explicit override
    if ($env:MSYS2_ROOT) {
        $p = Join-Path $env:MSYS2_ROOT "ucrt64\bin"
        if (Test-Path (Join-Path $p "g++.exe")) { return $p }
    }

    # 2) common install locations
    $candidates = @(
        "C:\msys64\ucrt64\bin",
        "D:\msys64\ucrt64\bin",
        "D:\worksoft\msys64\ucrt64\bin",
        "$env:USERPROFILE\msys64\ucrt64\bin",
        "$env:LOCALAPPDATA\msys64\ucrt64\bin"
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "g++.exe")) { return $c }
    }

    # 3) already on PATH? (e.g. running inside MSYS2 UCRT64 shell)
    $gcc = Get-Command "g++.exe" -ErrorAction SilentlyContinue
    if ($gcc) { return Split-Path -Parent $gcc.Source }
    $gcc = Get-Command "g++" -ErrorAction SilentlyContinue
    if ($gcc) { return Split-Path -Parent $gcc.Source }

    return $null
}

$Msys2Bin = Find-Msys2Bin
if ($Msys2Bin) {
    Write-Host "> using MSYS2 UCRT64 toolchain: $Msys2Bin"
    $env:PATH = "$Msys2Bin;$env:PATH"
    if (-not $env:CC)  { $env:CC  = "gcc" }
    if (-not $env:CXX) { $env:CXX = "g++" }
} else {
    Write-Warning "MSYS2 UCRT64 not found. Install from https://www.msys2.org"
    Write-Warning "Falling back to whatever compiler CMake auto-detects."
    Write-Warning "If you want MSVC, use scripts\build-msvc.ps1 instead."
}

# Make sure ninja is available
$ninja = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
if (-not $ninja) { $ninja = Get-Command "ninja" -ErrorAction SilentlyContinue }
if (-not $ninja) {
    Write-Warning "ninja not on PATH; CMake will fall back to its default generator."
    $Generator = $null
} else {
    $Generator = "Ninja"
}

# ── Mode → CMake options ─────────────────────────────────────────────────────
$DoCTest   = $false
$DoPackage = $false
$DoArchive = $false

# Defaults shared by every non-package mode: framework + tests, no examples.
# (Qt6 / AppKit adapters are auto-enabled below when the toolchain is present;
# adapter conformance tests belong to the framework core, examples don't.)
$CommonOpts = @(
    "-DARIA_BUILD_EXAMPLES=OFF"
)

switch ($Mode) {
    "clean" {
        Write-Host "> wiping $BuildDir/"
        if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
        exit 0
    }
    "android" {
        # Android NDK cross-build: configure with Android toolchain,
        # build the JNI adapter + core libraries.
        $AndroidSdkRoot = $env:ANDROID_SDK_ROOT
        if (-not $AndroidSdkRoot) {
            $candidates = @(
                "$env:LOCALAPPDATA\Android\Sdk",
                "C:\Android\Sdk",
                "D:\Android\Sdk"
            )
            foreach ($d in $candidates) {
                if (Test-Path $d) { $AndroidSdkRoot = $d; break }
            }
        }
        if (-not $AndroidSdkRoot) {
            Write-Error "ANDROID_SDK_ROOT not found. Set it or install Android SDK."
            exit 1
        }
        $AndroidNdkRoot = $env:ANDROID_NDK_ROOT
        if (-not $AndroidNdkRoot) {
            $ndkDir = Join-Path $AndroidSdkRoot "ndk"
            if (Test-Path $ndkDir) {
                $latestNdk = Get-ChildItem $ndkDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
                if ($latestNdk) { $AndroidNdkRoot = $latestNdk.FullName }
            }
        }
        if (-not $AndroidNdkRoot) {
            Write-Error "Android NDK not found at: $AndroidNdkRoot"
            exit 1
        }
        # Use SDK-bundled CMake
        $AndroidCmake = $env:ARIA_ANDROID_CMAKE
        if (-not $AndroidCmake) {
            $sdkCmakeDir = Join-Path $AndroidSdkRoot "cmake"
            if (Test-Path $sdkCmakeDir) {
                $latestCmake = Get-ChildItem $sdkCmakeDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
                if ($latestCmake) { $AndroidCmake = Join-Path $latestCmake.FullName "bin\cmake.exe" }
            }
        }
        if (-not $AndroidCmake) {
            $AndroidCmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
        }
        if (-not $AndroidCmake) {
            Write-Error "CMake not found. Install CMake or set ARIA_ANDROID_CMAKE."
            exit 1
        }
        Write-Host "> android NDK cross-build"
        Write-Host "  NDK   : $AndroidNdkRoot"
        Write-Host "  CMake : $AndroidCmake"
        $ToolchainFile = Join-Path $AndroidNdkRoot "build\cmake\android.toolchain.cmake"
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
        & $AndroidCmake -S . -B $BuildDir -G Ninja `
            -DCMAKE_BUILD_TYPE=Release `
            -DCMAKE_TOOLCHAIN_FILE=$ToolchainFile `
            -DANDROID_ABI=arm64-v8a `
            -DANDROID_PLATFORM=android-21 `
            -DARIA_BUILD_JNI=ON `
            -DARIA_BUILD_TESTS=ON `
            -DARIA_BUILD_EXAMPLES=OFF `
            -DARIA_BUILD_QT6=OFF
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $AndroidCmake --build $BuildDir -j $Jobs
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Host "[OK] android cross-build done"
        exit 0
    }
    { $_ -in "default", "release" } {
        $CMakeOpts = @("-DCMAKE_BUILD_TYPE=Release", "-DARIA_BUILD_TESTS=ON")
        $DoCTest   = $true
        $DoPackage = $true
    }
    "debug"   { $CMakeOpts = @("-DCMAKE_BUILD_TYPE=Debug", "-DARIA_BUILD_TESTS=ON") }
    "asan"    {
        $CMakeOpts = @("-DCMAKE_BUILD_TYPE=Debug",
                       "-DARIA_BUILD_TESTS=ON",
                       "-DARIA_ENABLE_ASAN=ON",
                       "-DARIA_ENABLE_UBSAN=ON")
    }
    "tests"   {
        $CMakeOpts = @("-DCMAKE_BUILD_TYPE=Release", "-DARIA_BUILD_TESTS=ON")
        $DoCTest = $true
    }
    { $_ -in "pack-zip", "pack", "zip", "tar", "package" } {
        $CMakeOpts = @("-DCMAKE_BUILD_TYPE=Release", "-DARIA_BUILD_TESTS=ON")
        $DoCTest   = $true
        $DoPackage = $true
        $DoArchive = $true
    }
    default {
        Write-Host "unknown mode: $Mode"
        Write-Host "valid: (none) | release | debug | tests | asan | pack-zip | android | clean"
        exit 1
    }
}

$CMakeOpts += $CommonOpts

# ── Auto-enable Qt6 adapter when Qt is found (qt6_tests is part of the core) ─
function Find-Qt6 {
    if ($env:ARIA_NO_QT6 -eq "1") { return $null }
    if ($env:QT_DIR) {
        if (Test-Path (Join-Path $env:QT_DIR "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $env:QT_DIR
        }
    }
    $roots = @("C:\Qt", "$env:USERPROFILE\Qt", "D:\Qt")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $versions = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^6\.' } |
                    Sort-Object Name -Descending
        foreach ($v in $versions) {
            foreach ($kit in @("mingw_64", "msvc2019_64", "msvc2022_64")) {
                $p = Join-Path $v.FullName $kit
                if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
                    return $p
                }
            }
        }
    }
    $msys2QtCandidates = @(
        "C:\msys64\ucrt64",
        "D:\msys64\ucrt64",
        "D:\worksoft\msys64\ucrt64"
    )
    foreach ($p in $msys2QtCandidates) {
        if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $p
        }
    }
    return $null
}

$Qt6Dir = Find-Qt6
if ($Qt6Dir) {
    Write-Host "> Qt6 detected at $Qt6Dir - adapter + qt6_tests enabled"
    $CMakeOpts += @("-DARIA_BUILD_QT6=ON", "-DCMAKE_PREFIX_PATH=$Qt6Dir")
}

# ── Configure ────────────────────────────────────────────────────────────────
Write-Host "> configuring ($Mode) with $Jobs jobs"
$ConfigureArgs = @("-S", ".", "-B", $BuildDir) + $CMakeOpts
if ($Generator) { $ConfigureArgs += @("-G", $Generator) }

# Use cmake.exe explicitly to avoid PATH resolution issues in some PowerShell environments
$cmakeCmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
if (-not $cmakeCmd) { $cmakeCmd = Get-Command "cmake" -ErrorAction SilentlyContinue }
if (-not $cmakeCmd) { Write-Error "cmake not found"; exit 1 }

& $cmakeCmd.Source @ConfigureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ── Build ────────────────────────────────────────────────────────────────────
Write-Host "> building"
& $cmakeCmd.Source --build $BuildDir -j $Jobs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ── Deploy DLL dependencies ──────────────────────────────────────────────────
# Borrowed from examples/1-qt-showcase/scripts/run.ps1: after every build,
# run windeployqt on Qt-using exes + iterative objdump to copy MinGW runtime
# DLLs into bin/.  Without this, launching any test exe by hand pops a
# "libgcc_s_seh-1.dll not found" / "Qt6Core.dll not found" dialog.
$absBin = Join-Path $BuildDir "bin"
if (Test-Path $absBin) {
    Write-Host "> deploying DLL dependencies"
    Deploy-DllDependencies -BinDir $absBin -Msys2Bin $Msys2Bin -Qt6Dir $Qt6Dir
}

# ── Test ─────────────────────────────────────────────────────────────────────
if ($DoCTest) {
    Write-Host "> running ctest"

    # Make the Qt6 bin dir visible on PATH as a belt-and-suspenders for any
    # DLL the loader still can't find (Deploy-DllDependencies already copies
    # Qt + plugins into bin/ via windeployqt; this is just insurance).
    if ($Qt6Dir) {
        $qtBin = Join-Path $Qt6Dir "bin"
        $env:PATH = "$qtBin;$env:PATH"
    }

    # Use a native-Windows ctest.exe, NOT the MSYS2 one.  MSYS2's ctest
    # applies POSIX path translation to the child-process environment,
    # which mangles PATH so the test exes can't find their DLLs (exit
    # code 0xc0000135 / STATUS_DLL_NOT_FOUND).  A native Win32 ctest
    # inherits PATH verbatim via CreateProcess.
    $ctestExe = $null
    # 1) Native CMake install (next to a non-MSYS2 cmake.exe)
    foreach ($c in (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
        if ($c.Source -match 'msys64|mingw') { continue }
        $probe = Join-Path (Split-Path $c.Source) "ctest.exe"
        if (Test-Path $probe) { $ctestExe = $probe; break }
    }
    # 2) Well-known native install dirs
    if (-not $ctestExe) {
        foreach ($p in @(
            "C:\Program Files\CMake\bin\ctest.exe",
            "C:\Program Files (x86)\CMake\bin\ctest.exe",
            "$env:LOCALAPPDATA\Programs\CMake\bin\ctest.exe"
        )) {
            if (Test-Path $p) { $ctestExe = $p; break }
        }
    }
    # 3) Last resort: whatever ctest is on PATH (may be MSYS2's)
    if (-not $ctestExe) { $ctestExe = "ctest" }
    Write-Host "> ctest: $ctestExe"
    # Same stderr-as-terminating-error trap as windeployqt above: ctest
    # writes "Errors while running CTest" to stderr even on a single test
    # failure.  Surface the output and rely on $LASTEXITCODE for pass/fail.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $ctestExe --test-dir $BuildDir --output-on-failure
    } catch {
        # Re-throw only if it's a genuine PowerShell error (not a native
        # stderr RemoteException).  Real native failures are caught by
        # the $LASTEXITCODE check below.
        if ($_.FullyQualifiedErrorId -ne "NativeCommandError") { throw }
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# ── Package ──────────────────────────────────────────────────────────────────
if ($DoPackage) {
    Write-Host "> packaging release"
    & $cmakeCmd.Source --build $BuildDir --target package-release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# ── Optional archive ─────────────────────────────────────────────────────────
if ($DoArchive) {
    Write-Host "> creating archive"
    & $cmakeCmd.Source --build $BuildDir --target package-archive
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host ""
    Write-Host "[OK] Archive created:"
    Get-ChildItem "build\dist\archives\*.*" -ErrorAction SilentlyContinue | ForEach-Object { "  $($_.FullName)" }
}

Write-Host "[OK] $Mode done"
} finally { Pop-Location }
