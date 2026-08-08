# examples/4-web-mvvm/scripts/run.ps1 - build & run the Web MVVM demo
#
# 用法：
#   examples\4-web-mvvm\scripts\run.ps1                        # Debug, HTTP, 19090, 自动开浏览器
#   examples\4-web-mvvm\scripts\run.ps1 Release                # Release, HTTP
#   examples\4-web-mvvm\scripts\run.ps1 Debug --tls            # Debug, HTTPS（自动生成自签名证书）
#   examples\4-web-mvvm\scripts\run.ps1 Debug --no-launch      # 只构建不运行
#   examples\4-web-mvvm\scripts\run.ps1 Debug --no-open        # 跑但不开浏览器
#   examples\4-web-mvvm\scripts\run.ps1 Debug --probe          # 启动 → 探测 → 退出
#   examples\4-web-mvvm\scripts\run.ps1 Debug --msvc           # 用 MSVC (Visual Studio 生成器)
#   examples\4-web-mvvm\scripts\run.ps1 Debug --msvc --tls     # MSVC + HTTPS
#
# Build dir : build/flavors/web-demo/      (MSYS2)  或  build/flavors/web-demo-msvc/  (MSVC)
#             两个工具链隔离，互不污染 CMakeCache
#             (build/examples/<name>/ is the main build's add_subdirectory
#              mirror and must not double as a standalone tree)

param(
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"

$UseTls  = $false
$UseMsvc = $false
$NoRun   = $false
$NoOpen  = $false
$Probe   = $false
foreach ($arg in $args) {
    if ($arg -eq "--tls")        { $UseTls = $true }
    if ($arg -eq "--msvc")       { $UseMsvc = $true }
    if ($arg -eq "--no-launch")  { $NoRun  = $true }
    if ($arg -eq "--no-run")     { $NoRun  = $true }
    if ($arg -eq "--no-open")    { $NoOpen = $true }
    if ($arg -eq "--probe")      { $Probe  = $true; $NoOpen = $true }
}

function Log-Info  { Write-Host "[demo4] $args" -ForegroundColor Blue }
function Log-Ok    { Write-Host "[demo4] $args" -ForegroundColor Green }
function Log-Warn  { Write-Host "[demo4] $args" -ForegroundColor Yellow }
function Log-Err   { Write-Host "[demo4] $args" -ForegroundColor Red }
function Log-Dim   { Write-Host "  $args" -ForegroundColor DarkGray }

$Port = if ($env:ARIA_DEMO4_PORT) { $env:ARIA_DEMO4_PORT } else { "19090" }
$Jobs = if ($env:JOBS) { $env:JOBS } else { [Environment]::ProcessorCount }

# -- 定位仓库根 ----------------------------------------------------------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DemoRoot  = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$RepoRoot  = (Resolve-Path (Join-Path $DemoRoot "../..")).Path
# MSVC 和 MSYS2 用不同的 build 目录，避免 CMakeCache 生成器冲突
$BuildDir  = if ($UseMsvc) {
    Join-Path $RepoRoot "build/flavors/web-demo-msvc"
} else {
    Join-Path $RepoRoot "build/flavors/web-demo"
}
# Demo builds STANDALONE against the shared installed SDK
# (build/flavors/sdk → build/dist/tree), same design as demo1.
$SdkTree   = Join-Path $RepoRoot "build/flavors/sdk"
$SdkPrefix = Join-Path $RepoRoot "build/dist/tree"

# ── 探测工具（Git Bash 环境需要 .exe 后缀） ──────────────────────────────
function Find-Cmd($name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $c = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

# ── 选择生成器 ───────────────────────────────────────────────────────────────
$Generator = $null
$Msys2Bin  = $null
$VsPath    = $null

if ($UseMsvc) {
    # MSVC 模式：从 vswhere 探测 Visual Studio (2022 / 2026 / ...)
    $vsWhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        $vsWhereAlt = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vsWhereAlt) { $vsWhere = $vsWhereAlt }
    }
    if ($env:ARIA_VS_GENERATOR) {
        $Generator = $env:ARIA_VS_GENERATOR
    } elseif (Test-Path $vsWhere) {
        $VsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if (-not $VsPath) {
            $VsPath = & $vsWhere -latest -products * -property installationPath 2>$null
        }
        $vsVer = & $vsWhere -latest -products * -property installationVersion 2>$null
        $vsName = & $vsWhere -latest -products * -property displayName 2>$null
        $vsMajor = $null; $vsYear = $null
        if ($vsVer -match '^(\d+)') { $vsMajor = $matches[1] }
        if ($vsName -match '(\d{4})\s*$') { $vsYear = $matches[1] }
        if ($vsMajor -and $vsYear) {
            $Generator = "Visual Studio $vsMajor $vsYear"
        }
    }
    if (-not $Generator) {
        $Generator = "Visual Studio 17 2022"
        Log-Warn "无法探测 VS 版本，回退到 $Generator"
    }
    # 清除 MSYS2 环境变量（INCLUDE/LIB/CPATH 会让 MSVC 混淆）
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

    # 注入 MSVC 编译环境（vcvars64.bat）。
    # Visual Studio 生成器自己能找 cl.exe，但 --tls 模式下 OpenSSL 用 nmake
    # 源码编译，nmake 需要 cl.exe/INCLUDE/LIB 在环境里。最可靠的方式是
    # 运行 vcvars64.bat 并把它的环境导入当前 PowerShell 进程。
    if ($VsPath) {
        $vcvars = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $vcvars) {
            Log-Info "注入 MSVC 环境: $vcvars"
            # 运行 vcvars64.bat && set，解析输出导入环境变量
            $envOutput = & cmd /c "`"$vcvars`" >nul 2>&1 && set" 2>&1
            foreach ($line in $envOutput) {
                if ($line -match '^([^=]+)=(.*)') {
                    [Environment]::SetEnvironmentVariable($matches[1], $matches[2])
                }
            }
        }
    }
} else {
    # MSYS2 模式：检测 UCRT64 工具链并注入 PATH
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
    $NinjaCmd = Find-Cmd ninja
    if ($NinjaCmd) {
        $Generator = "Ninja"
    }
}

if (-not (Find-Cmd cmake)) {
    Log-Err "cmake 未安装"
    exit 1
}

Log-Info "仓库根  : $RepoRoot"
Log-Info "构建目录: $BuildDir"
Log-Info "构建类型: $BuildType"
Log-Info "工具链  : $(if ($UseMsvc) { 'MSVC' } else { 'MSYS2 MinGW' })"
Log-Info "端口    : $Port"
Log-Info "TLS     : $(if ($UseTls) { '启用' } else { '关闭' })"
Log-Info "并行度  : $Jobs"
if ($Msys2Bin)   { Log-Info "MSYS2   : $Msys2Bin" }
if ($VsPath)     { Log-Info "VS      : $VsPath" }
if ($Generator)  { Log-Info "生成器  : $Generator" }

# ── Configure -----------------------------------------------------------------
$TlsFlag = if ($UseTls) { "ON" } else { "OFF" }

# TLS needs the OpenSSL git submodule (third_party/openssl/).
# Auto-init it when --tls is requested and the dir is empty.
if ($UseTls) {
    $opensslDir = Join-Path $RepoRoot "third_party\openssl"
    if ((Test-Path $opensslDir) -and (Get-ChildItem $opensslDir -ErrorAction SilentlyContinue | Measure-Object).Count -eq 0) {
        Log-Info "初始化 OpenSSL 子模块 (TLS 需要)..."
        # git-submodule runs inside Git-for-Windows' own mini-MSYS2
        # shell, so our PATH changes don't reach it.  Workaround: read
        # .gitmodules ourselves and git-clone directly.
        Push-Location $RepoRoot
        try {
            $subUrl = & git config -f .gitmodules submodule.third_party/openssl.url 2>$null
            if (-not $subUrl) {
                Log-Err "无法从 .gitmodules 读取 OpenSSL 子模块 URL"
                exit 1
            }
            Remove-Item $opensslDir -Force -ErrorAction SilentlyContinue
            # .gitmodules pins OpenSSL to 3.3.1 (commit db2ac4f).  Do NOT
            # blindly clone master — OpenSSL 4.x dev branches have a
            # Configure build-dir path bug that breaks MSYS2 MinGW builds.
            & git clone --depth 1 $subUrl "$opensslDir" 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0) {
                Push-Location "$opensslDir"
                try {
                    $pinnedCommit = "db2ac4f6ebd8f3d7b2a60882992fbea1269114e2"
                    & git fetch --depth 1 origin $pinnedCommit 2>&1 | Out-Null
                    & git checkout $pinnedCommit 2>&1 | Out-Null
                } finally { Pop-Location }
            }
        } catch { }
        Pop-Location
        if ($LASTEXITCODE -ne 0) {
            Log-Err "OpenSSL 克隆失败 (网络问题? 检查代理/防火墙/VPN)"
            Log-Err "手动修复步骤:"
            Log-Err "  1. git clone https://github.com/openssl/openssl.git third_party/openssl"
            Log-Err "  2. cd third_party/openssl ; git checkout db2ac4f  (OpenSSL 3.3.1)"
            Log-Err "  3. 或设置 Git 代理: git config --global http.proxy <你的代理>"
            exit 1
        }
        Log-Ok "OpenSSL 子模块就绪 (3.3.1 / db2ac4f)"
    }
}

# -- 确保框架 SDK 已构建并安装（与 demo1 共用；框架只构建一次）----------------
$SdkConfig = Join-Path $SdkPrefix "lib\cmake\aria\ariaConfig.cmake"
$NeedSdk = $false
if (-not (Test-Path $SdkConfig)) {
    $NeedSdk = $true
} else {
    $NewestSrc = Get-ChildItem -Path (Join-Path $RepoRoot "modules") -Recurse -File `
        -Include *.cpp,*.hpp,*.h,*.mm -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\tests\\|\\fuzz\\' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $NewestLib = (Get-Item $SdkConfig).LastWriteTime
    if ($NewestSrc -and $NewestSrc.LastWriteTime -gt $NewestLib) { $NeedSdk = $true }
}
if ($NeedSdk) {
    Log-Info "构建框架 SDK → $SdkPrefix ..."
    $sdkArgs = @(
        "-S", "$RepoRoot",
        "-B", "$SdkTree",
        "-DARIA_BUILD_QT6=ON",
        "-DARIA_BUILD_HTTP=ON",
        "-DARIA_BUILD_APPKIT=ON",
        "-DARIA_BUILD_EXAMPLES=OFF",
        "-DARIA_BUILD_TESTS=OFF",
        "-DARIA_BUILD_BENCHMARK=OFF"
    )
    if (-not $UseMsvc) { $sdkArgs += @("-DCMAKE_BUILD_TYPE=$BuildType") }
    if ($Generator) { $sdkArgs += @("-G", $Generator) }
    & cmake @sdkArgs 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Log-Err "SDK 配置失败"; exit 1 }
    & cmake --build "$SdkTree" --config "$BuildType" -j $Jobs 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Log-Err "SDK 编译失败"; exit 1 }
    # Full re-install: clear lib + include first so CMake never sees
    # up-to-date binaries and skips its install-time rpath rewrite
    # (same rationale as the macOS run.sh scripts).
    $SdkLib = Join-Path $SdkPrefix "lib"
    $SdkInc = Join-Path $SdkPrefix "include"
    if (Test-Path $SdkLib) { Remove-Item -Recurse -Force $SdkLib }
    if (Test-Path $SdkInc) { Remove-Item -Recurse -Force $SdkInc }
    & cmake --install "$SdkTree" --prefix "$SdkPrefix" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Log-Err "SDK 安装失败"; exit 1 }
    Log-Ok "框架 SDK 就绪：$SdkPrefix"
} else {
    Log-Ok "框架 SDK 已就绪：$SdkPrefix"
}

# -- Configure（standalone：只配 demo 自己，链接已安装 SDK）-------------------
Log-Info "配置 CMake..."
$cfgArgs = @(
    "-S", "$DemoRoot",
    "-B", "$BuildDir",
    "-DARIA_USE_INSTALLED=ON",
    "-DARIA_HTTP_ENABLE_TLS=$TlsFlag",
    "-DCMAKE_PREFIX_PATH=$SdkPrefix"
)
# MSYS2/Ninja 是单配置生成器，需要 -DCMAKE_BUILD_TYPE；
# MSVC Visual Studio 是多配置生成器，不需要（用 --config 在 build 时选）。
if (-not $UseMsvc) {
    $cfgArgs += @("-DCMAKE_BUILD_TYPE=$BuildType")
}
# OpenSSL 从 third_party/openssl 源码自闭环编译，不依赖系统 OpenSSL，
# 不需要传 -DOPENSSL_ROOT_DIR（BuildOpenSSL.cmake 自己管理安装路径）。
if ($Generator) { $cfgArgs += @("-G", $Generator) }
# Relax ErrorActionPreference: cmake writes benign warnings to stderr which
# PS5.1 treats as terminating errors under "Stop".
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& cmake @cfgArgs 2>&1 | Out-Null
$cfgExit = $LASTEXITCODE
$ErrorActionPreference = $prevEAP
if ($cfgExit -ne 0) { Log-Err "CMake 配置失败"; exit $cfgExit }

# -- Build ---------------------------------------------------------------------
Log-Info "编译 example_4_web_mvvm..."
# MSVC 多配置生成器用 --config 选 Debug/Release；Ninja 单配置忽略 --config。
# MSBuild v18 (VS 2026) MSB4166 workaround: MSVC 模式下用 /m:1 单进程。
$ErrorActionPreference = "Continue"
if ($UseMsvc) {
    $env:MSBUILDDISABLENODEREUSE = "1"
    & cmake --build "$BuildDir" --target example_4_web_mvvm --config "$BuildType" -- /m:1 2>&1 | Out-Null
} else {
    & cmake --build "$BuildDir" --target example_4_web_mvvm --config "$BuildType" -j $Jobs 2>&1 | Out-Null
}
$buildExit = $LASTEXITCODE
$ErrorActionPreference = $prevEAP
if ($buildExit -ne 0) { Log-Err "编译失败"; exit 1 }

# ── 探测产物路径（不同生成器输出位置不同） ────────────────────────────────────
# Ninja:        bin/example_4_web_mvvm.exe
# Visual Studio: bin/<Config>/example_4_web_mvvm.exe  (多配置子目录)
$FinalAppPath = $null
foreach ($p in @(
    (Join-Path $BuildDir "$BuildType\example_4_web_mvvm.exe"),
    (Join-Path $BuildDir "example_4_web_mvvm.exe"),
    (Join-Path $BuildDir "example_4_web_mvvm")
)) {
    if (Test-Path $p) { $FinalAppPath = $p; break }
}

if (-not $FinalAppPath) {
    Log-Err "构建成功但找不到可执行文件：$FinalAppPath"
    exit 1
}
Log-Ok "构建完成：$FinalAppPath"

# -- 复制运行时依赖 DLL 到 exe 目录 --------------------------------------------
# Standalone 模式下 exe 动态链接 SDK 的 libaria_*.dll 和 MinGW 运行时
# (libstdc++-6.dll 等)。不复制的话 exe 双击/脚本启动都会因缺 DLL 直接退出。
function Copy-RuntimeDeps($exePath, $destDir, $msys2Bin) {
    # 1) 项目自身 DLL（来自 SDK 安装树 bin/）
    $projectDlls = Get-ChildItem -Path (Join-Path $SdkPrefix "bin") -Filter "libaria_*.dll" -ErrorAction SilentlyContinue
    foreach ($dll in $projectDlls) {
        Copy-Item -Path $dll.FullName -Destination $destDir -Force
        Log-Dim "  copied: $($dll.Name)"
    }

    # 2) 用 objdump 迭代复制 MSYS2 bin 中的依赖（MinGW runtime + 传递依赖）
    if (-not $msys2Bin) { return }
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
                    Log-Dim "  copied: $dllName"
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

$ExeDir = Split-Path -Parent $FinalAppPath
Copy-RuntimeDeps -exePath $FinalAppPath -destDir $ExeDir -msys2Bin $Msys2Bin

if ($NoRun) {
    Log-Info "--no-launch 已指定，不启动"
    exit 0
}

# -- 自签名证书（如启用 TLS 且未提供）------------------------------------------
$CertPath = ""
$KeyPath  = ""
if ($UseTls) {
    $CertPath = if ($env:ARIA_DEMO4_CERT) { $env:ARIA_DEMO4_CERT } else { "" }
    $KeyPath  = if ($env:ARIA_DEMO4_KEY)  { $env:ARIA_DEMO4_KEY  } else { "" }
    if (-not $CertPath -or -not $KeyPath) {
        $CertDir = Join-Path $BuildDir "_certs"
        New-Item -ItemType Directory -Force -Path $CertDir | Out-Null
        $CertPath = Join-Path $CertDir "cert.pem"
        $KeyPath  = Join-Path $CertDir "key.pem"
        if (-not (Test-Path $CertPath) -or -not (Test-Path $KeyPath)) {
            Log-Info "生成自签名证书 → $CertDir"
            & openssl req -x509 -newkey rsa:2048 -nodes `
                -keyout $KeyPath -out $CertPath `
                -days 365 -subj "/CN=localhost" 2>$null
        }
    }
    Log-Info "证书    : $CertPath"
    Log-Info "私钥    : $KeyPath"
}

$Scheme = if ($UseTls) { "https" } else { "http" }
$Url = "${Scheme}://127.0.0.1:$Port"

# -- 启动服务器（始终启用 static_root → 浏览器同源）----------------------------
Log-Info "启动 -> $Url  (static_root = $DemoRoot)"
# ── 修复 PowerShell 5.1 Start-Process 的 Path/PATH 大小写冲突 bug ──────────
# 环境块里存在多个 PATH 大小写变体时（WorkBuddy 等父进程创建），Start-Process
# 的 StringDictionary 会抛 ArgumentException "已添加项"。合并到单个大写 PATH。
$pathVariants = [System.Environment]::GetEnvironmentVariables().Keys | Where-Object { $_ -ieq "path" }
if ($pathVariants.Count -gt 1) {
    $pathValue = [Environment]::GetEnvironmentVariable("PATH")
    foreach ($v in $pathVariants) {
        [Environment]::SetEnvironmentVariable($v, $null)
    }
    [Environment]::SetEnvironmentVariable("PATH", $pathValue)
}
if ($UseTls) {
    $proc = Start-Process -FilePath $FinalAppPath -ArgumentList @($DemoRoot, $CertPath, $KeyPath) `
        -PassThru -NoNewWindow
} else {
    $proc = Start-Process -FilePath $FinalAppPath -ArgumentList @($DemoRoot) `
        -PassThru -NoNewWindow
}

try {
    # 等监听就绪（最多 5s）
    $ListenerOk = $false
    for ($i = 0; $i -lt 10; $i++) {
        try {
            $r = & curl.exe -sk --max-time 1 "$Url/aria/health" 2>$null
            if ($r) { $ListenerOk = $true; break }
        } catch {}
        Start-Sleep -Milliseconds 500
    }
    if (-not $ListenerOk) {
        Log-Err "服务器启动失败：$Url/aria/health 无响应"
        exit 1
    }
    Log-Ok "服务器已就绪：$Url"

    if ($Probe) {
        Log-Info "probe 模式：curl 各端点 → 退出"
        $ProbeOk = $true
        foreach ($ep in @("/aria/health", "/aria/views")) {
            $out = & curl.exe -sk --max-time 2 "$Url$ep"
            if (-not $out) {
                Log-Err "probe 失败：$Url$ep 无响应"
                $ProbeOk = $false
            } else {
                Log-Ok "$ep → $out"
            }
        }
        if (-not $ProbeOk) { exit 1 }
        Log-Ok "probe 通过"
        exit 0
    }

    $DemoUrl = "$Url/index.html"
    if (-not $NoOpen) {
        Log-Info "打开浏览器 → $DemoUrl"
        Start-Process $DemoUrl | Out-Null
    } else {
        Log-Info "（手动访问 $DemoUrl）"
    }

    Log-Info "（Ctrl-C 退出）"
    Wait-Process -Id $proc.Id
}
finally {
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
}
