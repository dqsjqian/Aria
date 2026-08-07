# run-msvc.ps1 - Standalone VS solution for Qt showcase demo
#
# 不依赖主 Aria 源码树，link 的是 `build-msvc.ps1` 产出的 framework
# (位于 build/dist/tree/)。
# 默认只生成 .sln，在 VS 里手动编译调试。
#
# 前提：先跑过 scripts\build-msvc.ps1（生成 build/dist/tree/）
#
# 用法：
#   .\run-msvc.ps1                  # 默认：生成 VS 工程到 build/flavors/qt-demo-msvc/
#   .\run-msvc.ps1 Debug            # Debug 编译 + windeployqt + 运行
#   .\run-msvc.ps1 Release          # Release 编译 + 运行
#   .\run-msvc.ps1 Debug probe      # Debug + probe 模式
#   .\run-msvc.ps1 Debug --no-run   # 只编译不运行
#
# 环境变量：
#   $env:QT_DIR   Qt 路径（如 D:\worksoft\Qt\6.11.1\msvc2022_64）
#   $env:JOBS     并行任务数
#   $env:ARIA_VS_GENERATOR  覆盖 CMake 生成器（如 "Visual Studio 18 2026"）

param(
    [string]$BuildType = "Release",
    [string]$Mode     = "normal"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
# examples/1-qt-showcase/scripts → go up 3 levels to repo root
$RepoRoot  = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ScriptDir))
$DemoSrc   = Split-Path -Parent $ScriptDir  # examples/1-qt-showcase
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
    # 同 build-msvc.ps1 中的说明：环境块里多个 PATH 大小写变体会导致
    # MSBuild 的 StringDictionary 抛 ArgumentException，CL.exe 无法启动。
    $pathVariants = [System.Environment]::GetEnvironmentVariables().Keys | Where-Object { $_ -ieq "path" }
    if ($pathVariants.Count -gt 1) {
        $pathValue = [Environment]::GetEnvironmentVariable("PATH")
        foreach ($v in $pathVariants) {
            [Environment]::SetEnvironmentVariable($v, $null)
        }
        [Environment]::SetEnvironmentVariable("PATH", $pathValue)
    }

    Set-Location $RepoRoot

    $BuildDir   = "build/flavors/qt-demo-msvc"
    # Standalone demo tree: links the framework install under
    # build/dist/tree/ (produced by scripts\build-msvc.ps1), so the demo
    # does NOT recompile the framework. Lives under build/flavors/ per the
    # unified layout — build/examples/<name>/ is the main build's
    # add_subdirectory mirror and must not double as a standalone tree.
    $FrameworkRelease = "build/dist/tree"

    # -- 探测 Visual Studio 生成器 (支持 VS 2022 / 2026 / ...) ------------------
    # 从 vswhere 拿 VS 主版本号 + 年份，组合成 "Visual Studio <major> <year>"。
    # 不硬编码 "Visual Studio 17 2022" —— 本机可能只装了 VS 2026。
    $vsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        $vsWhereAlt = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vsWhereAlt) { $vsWhere = $vsWhereAlt }
    }
    $script:VsPath = $null   # 保存供后面 VC runtime DLL 拷贝用
    if ($env:ARIA_VS_GENERATOR) {
        $Generator = $env:ARIA_VS_GENERATOR
    } elseif (Test-Path $vsWhere) {
        $script:VsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if (-not $script:VsPath) {
            $script:VsPath = & $vsWhere -latest -products * -property installationPath 2>$null
        }
        $vsVer = & $vsWhere -latest -products * -property installationVersion 2>$null
        $vsName = & $vsWhere -latest -products * -property displayName 2>$null
        $vsMajor = $null; $vsYear = $null
        if ($vsVer -match '^(\d+)') { $vsMajor = $matches[1] }
        if ($vsName -match '(\d{4})\s*$') { $vsYear = $matches[1] }
        if ($vsMajor -and $vsYear) {
            $Generator = "Visual Studio $vsMajor $vsYear"
        } else {
            $Generator = "Visual Studio 17 2022"
            Log-Warn "无法从 vswhere 解析 VS 版本，回退到 $Generator"
        }
    } else {
        $Generator = "Visual Studio 17 2022"
        Log-Warn "vswhere 未找到，回退到 $Generator"
    }
    Log-Info "生成器  : $Generator"

    # -- 解析额外参数 ------------------------------------------------------------
    $NoRun = $false
    foreach ($arg in $args) {
        if ($arg -eq "--no-run") { $NoRun = $true }
    }
    if ($Mode -eq "--no-run") { $NoRun = $true; $Mode = "normal" }

    # -- 颜色 --------------------------------------------------------------------
    function Log-Info  { Write-Host "[demo1] $args" -ForegroundColor Cyan }
    function Log-Ok    { Write-Host "[demo1] $args" -ForegroundColor Green }
    function Log-Warn  { Write-Host "[demo1] $args" -ForegroundColor Yellow }
    function Log-Err   { Write-Host "[demo1] $args" -ForegroundColor Red }
    function Log-Dim   { Write-Host "  $args" -ForegroundColor DarkGray }

    $AppPath = Join-Path $BuildDir "$BuildType\ex_qt_showcase.exe"

    # -- 并行度 ------------------------------------------------------------------
    $Jobs = if ($env:JOBS) { $env:JOBS } else { $env:NUMBER_OF_PROCESSORS }
    if (-not $Jobs) { $Jobs = 4 }

    # -- 前提：framework 必须已构建 ----------------------------------------------
    # 支持两种安装路径：旧版 cmake/ 和标准的 lib/cmake/aria/
    $AriaConfig = Join-Path $FrameworkRelease "cmake\ariaConfig.cmake"
    $AriaConfigStd = Join-Path $FrameworkRelease "lib\cmake\aria\ariaConfig.cmake"
    if (Test-Path $AriaConfigStd) {
        $AriaConfig = $AriaConfigStd
    } elseif (-not (Test-Path $AriaConfig)) {
        Log-Err "未找到 framework release: $AriaConfig"
        Log-Err "请先运行:  .\scripts\build-msvc.ps1"
        exit 1
    }

    # -- CMake -------------------------------------------------------------------
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $cmake) { $cmake = Get-Command cmake -ErrorAction SilentlyContinue }
    if (-not $cmake) { Log-Err "cmake 未安装。https://cmake.org/download/"; exit 1 }
    $cmakePath = $cmake.Source

    # -- Qt6 ---------------------------------------------------------------------
    if ($env:QT_DIR) {
        $QtDir = $env:QT_DIR
        if (-not (Test-Path (Join-Path $QtDir "lib\cmake\Qt6\Qt6Config.cmake"))) {
            Log-Warn "QT_DIR 不是合法 Qt6: $QtDir"; $QtDir = $null
        }
    } else {
        $QtDir = $null
        $roots = @("D:\worksoft\Qt", "C:\Qt", "D:\Qt")
        $kitOrder = @("msvc2022_64", "msvc2019_64", "mingw_64")
        foreach ($root in $roots) {
            if (-not (Test-Path $root)) { continue }
            $vers = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^6\.' } | Sort-Object Name -Descending
            foreach ($v in $vers) {
                foreach ($kit in $kitOrder) {
                    $p = Join-Path $v.FullName $kit
                    if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
                        $QtDir = $p; break
                    }
                }
                if ($QtDir) { break }
            }
            if ($QtDir) { break }
        }
    }
    if (-not $QtDir) { Log-Err "未找到 Qt 6.x (msvc2022_64)。请安装或设置 `$env:QT_DIR"; exit 1 }

    Log-Info "仓库根  : $RepoRoot"
    Log-Info "构建类型: $BuildType"
    Log-Info "Framework: $FrameworkRelease"
    Log-Info "Qt 路径 : $QtDir"
    Log-Info "并行度  : $Jobs"
    Log-Info ""

    # -- 缓存冲突检测 ----------------------------------------------------------
    $CacheFile = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $CacheFile) {
        $oldGen = Select-String -Path $CacheFile -Pattern '^CMAKE_GENERATOR:INTERNAL=' 2>$null | Select-Object -First 1
        if ($oldGen) {
            $oldGen = $oldGen.Line -replace '^CMAKE_GENERATOR:INTERNAL=',''
            if ($oldGen -ne $Generator) {
                Log-Warn "缓存生成器是 '$oldGen'，不能和 '$Generator' 混用，自动清理 build/"
                Remove-Item -Recurse -Force $BuildDir -ErrorAction Stop
            }
        }
    }

    # -- Configure (standalone) --------------------------------------------------
    # Build against the INSTALLED aria SDK, not the source tree.
    # CMAKE_PREFIX_PATH: framework release (aria) + Qt
    $prefixPath = "$RepoRoot\$FrameworkRelease;$QtDir"
    Log-Info "配置 CMake (standalone)..."
    $cfgArgs = @(
        "-S", $DemoSrc,
        "-B", $BuildDir,
        "-G", $Generator,
        "-DARIA_USE_INSTALLED=ON",
        "-DCMAKE_PREFIX_PATH=$prefixPath"
    )
    & $cmakePath @cfgArgs
    if ($LASTEXITCODE -ne 0) { Log-Err "CMake 配置失败"; exit $LASTEXITCODE }

    if ($BuildType -eq "configure") {
        Log-Ok "VS 工程已生成 (standalone)。"
        Write-Host "  解决方案: $RepoRoot\$BuildDir\aria-qt-showcase.sln"
        Write-Host "  用 VS 2022 打开 -> 选择 Release -> 编译 ex_qt_showcase"
        exit 0
    }

    # -- Build -------------------------------------------------------------------
    Log-Info "编译 ex_qt_showcase ($BuildType)..."
    # MSBuild v18 (VS 2026) MSB4166 workaround: force /m:1 (see build-msvc.ps1)
    $env:MSBUILDDISABLENODEREUSE = "1"
    & $cmakePath --build $BuildDir --config $BuildType -- /m:1
    if ($LASTEXITCODE -ne 0) { Log-Err "编译失败"; exit $LASTEXITCODE }

    if (-not (Test-Path $AppPath)) {
        Log-Err "构建成功但找不到: $AppPath"
        exit 1
    }
    Log-Ok "构建完成: $AppPath"

    # -- 部署 DLL (framework + windeployqt + VC runtime) ---------------------------
    $ExeDir = Split-Path -Parent $AppPath

    # Aria framework DLLs (from build/dist/tree/bin/)
    $AriaDllDir = Join-Path $FrameworkRelease "bin"
    if (Test-Path $AriaDllDir) {
        Log-Info "Copying Aria DLLs..."
        Copy-Item "$AriaDllDir\*.dll" $ExeDir -Force
    }

    # Qt DLLs via windeployqt
    $qtBin  = Join-Path $QtDir "bin"
    $windeployqt = Join-Path $qtBin "windeployqt.exe"
    if (Test-Path $windeployqt) {
        Log-Info "windeployqt..."
        $oldEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $windeployqt $AppPath --no-compiler-runtime
        $ErrorActionPreference = $oldEAP
    }

    # VC++ runtime DLLs — 用前面 vswhere 探测到的 VS 路径，不硬编码 "2022"。
    # 回退顺序：探测到的 VSPath → $env:VS_INSTALL_DIR → Program Files 下 2022/2026 通配。
    $vcDlls = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll")
    $vcRedistDirs = @()
    if ($script:VsPath) {
        $msvcDirs = Get-ChildItem "$($script:VsPath)\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($msvcDirs) {
            $vcRedistDirs += Join-Path $msvcDirs[0].FullName "bin\Hostx64\x64"
        }
    }
    if ($env:VS_INSTALL_DIR) {
        $customMsvc = Get-ChildItem "$env:VS_INSTALL_DIR\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($customMsvc) {
            $vcRedistDirs += Join-Path $customMsvc[0].FullName "bin\Hostx64\x64"
        }
    }
    # 最后兜底：Program Files 下任意 VS 版本（2022 或 2026）
    if (-not $vcRedistDirs) {
        $pfMsvc = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\202*\*\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($pfMsvc) {
            $vcRedistDirs += Join-Path $pfMsvc[0].FullName "bin\Hostx64\x64"
        }
    }
    foreach ($dll in $vcDlls) {
        if (Test-Path (Join-Path $ExeDir $dll)) { continue }
        foreach ($dir in $vcRedistDirs) {
            $src = Join-Path $dir $dll
            if (Test-Path $src) { Copy-Item $src $ExeDir -Force; Log-Dim "copied: $dll"; break }
        }
    }

    # -- Run ---------------------------------------------------------------------
    if ($NoRun) { Log-Info "--no-run 已指定，不启动"; exit 0 }

    if ($Mode -eq "probe") {
        Log-Info "probe 模式启动..."
        $env:ARIA_PROBE = "1"
        & $AppPath
        Remove-Item Env:\ARIA_PROBE -ErrorAction SilentlyContinue
    } else {
        Log-Info "启动 GUI..."
        & $AppPath
    }

} finally {
    Set-Location $OriginalDir
}
