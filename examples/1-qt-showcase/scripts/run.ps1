# examples/1-qt-showcase/scripts/run.ps1 - build & run the Qt showcase demo
#
# 用法：
#   examples\1-qt-showcase\scripts\run.ps1                        # Debug，弹 GUI
#   examples\1-qt-showcase\scripts\run.ps1 Release                # Release，弹 GUI
#   examples\1-qt-showcase\scripts\run.ps1 Debug probe            # Debug + probe 模式（跑完 9 tab 自动退出）
#   examples\1-qt-showcase\scripts\run.ps1 Debug --no-launch      # 只构建不运行
#
# 环境变量：
#   $env:QT_DIR          自定义 Qt 路径，例：
#                            C:\Qt\6.6.0\mingw_64
#                            C:\Qt\6.6.0\msvc2022_64
#                        不设则自动扫 C:\Qt\6.x\* / %USERPROFILE%\Qt\6.x\*
#   $env:JOBS            并行任务数（默认 CPU 核心数）
#
# 前置依赖：
#   - CMake ≥ 3.20       (https://cmake.org)
#   - Qt 6.x             (https://www.qt.io/download-qt-installer)
#   - MSYS2 UCRT64 + MinGW GCC（MSVC 不在支持范围）

param(
    [string]$BuildType = "Debug",
    [string]$Mode = "normal"
)

$ErrorActionPreference = "Stop"

$NoRun = $false
foreach ($arg in $args) {
    if ($arg -eq "--no-launch" -or $arg -eq "--no-run") { $NoRun = $true }
}
# 如果 $Mode 本身就是 --no-launch / --no-run，也纳入
if ($Mode -eq "--no-launch" -or $Mode -eq "--no-run") { $NoRun = $true; $Mode = "normal" }

# -- 颜色 ----------------------------------------------------------------------
function Log-Info  { Write-Host "[demo1] $args" -ForegroundColor Blue }
function Log-Ok    { Write-Host "[demo1] $args" -ForegroundColor Green }
function Log-Warn  { Write-Host "[demo1] $args" -ForegroundColor Yellow }
function Log-Err   { Write-Host "[demo1] $args" -ForegroundColor Red }
function Log-Dim   { Write-Host "  $args" -ForegroundColor DarkGray }

# -- 复制 DLL 依赖到 exe 目录 --------------------------------------------------
function Copy-Dependencies($exePath, $destDir, $msys2Bin) {
    if (-not $msys2Bin) { return }

    # 1) 复制项目自身的 DLL（优先从统一输出目录 bin/ 找，其次回退到 modules/）
    $projectDlls = Get-ChildItem -Path "$BuildDir/bin" -Filter "libaria_*.dll" -ErrorAction SilentlyContinue
    if (-not $projectDlls) {
        $projectDlls = Get-ChildItem -Path "$BuildDir/modules" -Filter "libaria_*.dll" -Recurse -ErrorAction SilentlyContinue
    }
    foreach ($dll in $projectDlls) {
        $dest = Join-Path $destDir $dll.Name
        if (-not (Test-Path $dest)) {
            Copy-Item -Path $dll.FullName -Destination $destDir -Force
            Log-Dim "  copied project: $($dll.Name)"
        }
    }

    # 2) windeployqt 复制 Qt DLL + 插件
    $windeployqt = Join-Path $msys2Bin "windeployqt.exe"
    if (Test-Path $windeployqt) {
        Log-Info "windeployqt 复制 Qt 依赖..."
        Push-Location $destDir
        & $windeployqt $exePath
        Pop-Location
    }

    # 3) 用 objdump 迭代复制 ucrt64/bin 中的剩余依赖（MinGW runtime + Qt 的 MSYS2 依赖）
    $objdump = Join-Path $msys2Bin "objdump.exe"
    if (-not (Test-Path $objdump)) { return }

    function Get-Imports($filePath) {
        $tmp = [System.IO.Path]::GetTempFileName()
        & $objdump -p $filePath > $tmp 2>$null
        $output = Get-Content $tmp
        Remove-Item $tmp
        $imports = @()
        foreach ($line in $output) {
            if ($line -match "DLL Name:\s*(.+)") {
                $imports += $matches[1].Trim()
            }
        }
        return $imports
    }

    $filesToScan = @($exePath)
    $processed = @{}

    do {
        $newFiles = @()
        foreach ($file in $filesToScan) {
            $imports = Get-Imports $file
            foreach ($dllName in $imports) {
                if ($processed.ContainsKey($dllName)) { continue }

                $destPath = Join-Path $destDir $dllName
                if (Test-Path $destPath) {
                    $processed[$dllName] = $true
                    $newFiles += $destPath
                    continue
                }

                $srcPath = Join-Path $msys2Bin $dllName
                if (Test-Path $srcPath) {
                    Copy-Item -Path $srcPath -Destination $destDir -Force
                    Log-Dim "  copied dep: $dllName"
                    $processed[$dllName] = $true
                    $newFiles += $srcPath
                } else {
                    $processed[$dllName] = $true
                }
            }
        }
        $filesToScan = $newFiles
    } while ($filesToScan.Count -gt 0)
}

# -- 定位仓库根 ----------------------------------------------------------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DemoRoot  = Split-Path -Parent $ScriptDir
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $DemoRoot)
$BuildDir  = Join-Path $RepoRoot "build/flavors/qt-demo"
# Per-demo isolated build tree under build/flavors/<name>-demo/ so each
# demo's cmake cache does not collide with framework or other demos' configs.
# (build/examples/<name>/ is the main build's add_subdirectory mirror and
# must NOT double as a standalone tree — see scripts/build.sh layout.)
$AppPath   = Join-Path $BuildDir "$BuildType\ex_qt_showcase.exe"

# 某些 Windows 生成器（Ninja、Unix Makefiles）不分 Debug/Release 子目录
$AppPathFlat = Join-Path $BuildDir "ex_qt_showcase.exe"

# 项目设置了 CMAKE_RUNTIME_OUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/bin，exe 可能在这里
$AppPathBin = Join-Path $BuildDir "bin\ex_qt_showcase.exe"

# -- 并行度 --------------------------------------------------------------------
$Jobs = if ($env:JOBS) { $env:JOBS } else { $env:NUMBER_OF_PROCESSORS }
if (-not $Jobs) { $Jobs = 4 }

# -- 探测工具（Git Bash 环境需要 .exe 后缀） ------------------------------------
function Find-Cmd($name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $c = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

# -- 探测 Qt 路径 --------------------------------------------------------------
function Find-Qt6 {
    if ($env:QT_DIR) {
        if (Test-Path (Join-Path $env:QT_DIR "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $env:QT_DIR
        }
        Log-Warn "QT_DIR 不是合法 Qt6 安装根：$($env:QT_DIR)"
    }

    # 1) 标准 Qt 安装器路径
    $roots = @("C:\Qt", "D:\Qt", "$env:USERPROFILE\Qt")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $versions = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^6\.' } |
                    Sort-Object Name -Descending
        foreach ($v in $versions) {
            foreach ($kit in @("mingw_64", "msvc2022_64", "msvc2019_64")) {
                $p = Join-Path $v.FullName $kit
                if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
                    return $p
                }
            }
        }
    }

    # 2) MSYS2 UCRT64 pacman 安装的 Qt6
    $msys2Candidates = @("C:\msys64\ucrt64", "D:\msys64\ucrt64", "D:\worksoft\msys64\ucrt64")
    foreach ($p in $msys2Candidates) {
        if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $p
        }
    }

    return $null
}

$QtDir = Find-Qt6

Log-Info "仓库根  : $RepoRoot"
Log-Info "构建类型: $BuildType"
Log-Info "Qt 路径 : $(if ($QtDir) { $QtDir } else { '未找到' })"
Log-Info "并行度  : $Jobs"

# -- 检测 MSYS2 工具链并注入 PATH ---------------------------------------------
$Msys2Bin = $null
$candidates = @("C:\msys64\ucrt64\bin", "D:\msys64\ucrt64\bin", "D:\worksoft\msys64\ucrt64\bin")
foreach ($c in $candidates) {
    if (Test-Path (Join-Path $c "g++.exe")) { $Msys2Bin = $c; break }
}
if (-not $Msys2Bin) {
    $gpp = Find-Cmd "g++"
    if ($gpp) { $Msys2Bin = Split-Path -Parent $gpp }
}
if ($Msys2Bin) {
    $env:PATH = "$Msys2Bin;$env:PATH"
}

# -- 预检 ----------------------------------------------------------------------
$CmakePath = Find-Cmd "cmake"
if (-not $CmakePath) {
    Log-Err "cmake 未安装。请访问 https://cmake.org/download/ 下载安装"
    exit 1
}
if (-not $QtDir) {
    Log-Err "未找到 Qt 6.x。请从 https://www.qt.io/download-qt-installer 安装"
    Log-Err "或设置  `$env:QT_DIR = 'C:\path\to\qt'"
    exit 1
}

# -- 如果是 MinGW/MSYS2 kit，优先用 Ninja 生成器（比 NMake/VS 更快更稳） ------
$Generator = $null
$IsMingwLike = ($QtDir -match "mingw") -or ($QtDir -match "msys64")
if ($IsMingwLike) {
    if (Find-Cmd "ninja") {
        $Generator = "Ninja"
    } else {
        $Generator = "MinGW Makefiles"
    }
    # 把 MinGW 的 bin 塞进 PATH（Qt 安装时通常附带）
    $qtMingwBin = Join-Path (Split-Path $QtDir -Parent) "mingw_64\bin"
    $qtToolsMingw = Join-Path (Split-Path (Split-Path $QtDir -Parent) -Parent) "Tools"
    foreach ($p in @($qtMingwBin, $qtToolsMingw)) {
        if (Test-Path $p) { $env:PATH = "$p;$env:PATH" }
    }
}

# -- Configure -----------------------------------------------------------------
Log-Info "配置 CMake..."
$cfgArgs = @(
    "-S", $RepoRoot,
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_PREFIX_PATH=$QtDir",
    "-DARIA_BUILD_QT6=ON",
    "-DARIA_BUILD_EXAMPLES=ON",
    "-DARIA_BUILD_TESTS=OFF"
)
if ($Generator) { $cfgArgs += @("-G", $Generator) }
& $CmakePath @cfgArgs
if ($LASTEXITCODE -ne 0) { Log-Err "CMake 配置失败"; exit $LASTEXITCODE }

# -- Build ---------------------------------------------------------------------
Log-Info "编译 ex_qt_showcase..."
$buildArgs = @("--build", $BuildDir, "--target", "ex_qt_showcase", "--config", $BuildType, "-j", $Jobs)
& $CmakePath @buildArgs
if ($LASTEXITCODE -ne 0) { Log-Err "编译失败"; exit $LASTEXITCODE }

# -- 找到产物（VS 生成器有 Debug/ 子目录，Ninja/Makefile 没有） ----------------
$FinalAppPath = $null
foreach ($p in @($AppPath, $AppPathFlat, $AppPathBin)) {
    if (Test-Path $p) { $FinalAppPath = $p; break }
}
if (-not $FinalAppPath) {
    Log-Err "构建成功但没找到可执行文件。候选路径："
    Log-Err "  $AppPath"
    Log-Err "  $AppPathFlat"
    exit 1
}
Log-Ok "构建完成：$FinalAppPath"

# -- 复制依赖 DLL --------------------------------------------------------------
$ExeDir = Split-Path -Parent $FinalAppPath
Copy-Dependencies -exePath $FinalAppPath -destDir $ExeDir -msys2Bin $Msys2Bin

# -- Run -----------------------------------------------------------------------
if ($NoRun) {
    Log-Info "--no-launch 已指定，不启动应用"
    exit 0
}

if ($Mode -eq "probe") {
    Log-Info "以 probe 模式启动（跑完 9 tab 自动退出）..."
    $env:ARIA_PROBE = "1"
    & $FinalAppPath
    Remove-Item Env:\ARIA_PROBE -ErrorAction SilentlyContinue
} else {
    Log-Info "启动 GUI..."
    & $FinalAppPath
}
