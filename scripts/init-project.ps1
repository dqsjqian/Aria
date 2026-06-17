# init-project.ps1 - bootstrap a freshly-cloned Aria checkout for Windows dev.
#
# What it does:
#   1. Detects the platform (Windows expected; other OSes are pointed at init-project.sh).
#   2. Auto-detects Qt6 (scans C:\Qt\6.x\mingw_64 etc.).
#   3. Auto-detects CMake and the compiler (MinGW via MSYS2 UCRT64).
#   4. If any required dependency is missing, prints a full install
#      walkthrough and exits WITHOUT generating .vscode/.
#   5. Generates .vscode/{settings,tasks,launch,extensions,c_cpp_properties}.json
#      with absolute paths populated from this machine.
#   6. Creates build/ so VSCode's relative paths resolve up-front.
#
# Usage:
#   .\scripts\init-project.ps1                            # default: overwrite .vscode/*
#   .\scripts\init-project.ps1 -NoOverwrite               # skip if a file already exists
#   .\scripts\init-project.ps1 -DryRun                    # preview only, do not write
#   $env:QT_DIR="C:\Qt\6.6.0\mingw_64"; .\scripts\init-project.ps1
#   $env:ARIA_NO_QT6="1"; .\scripts\init-project.ps1       # skip Qt (don't build demo1)
#
# Re-entry safety: .vscode/*.json are template-generated; every run
# refreshes them to the latest template. Don't hand-edit those JSONs -
# update the templates in this script instead.

param(
    [switch]$NoOverwrite,
    [switch]$DryRun,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help) {
    Get-Content $MyInvocation.MyCommand.Path | Select-Object -First 20
    exit 0
}

# -- Logging ------------------------------------------------------------------
function Log-Info  { Write-Host "[init] $args" -ForegroundColor Blue }
function Log-Ok    { Write-Host "[ ok] $args" -ForegroundColor Green }
function Log-Warn  { Write-Host "[ ! ] $args" -ForegroundColor Yellow }
function Log-Err   { Write-Host "[err] $args" -ForegroundColor Red }
function Log-Dim   { Write-Host "  $args" -ForegroundColor DarkGray }

# Save the caller's current directory and restore it on exit.
$OriginalDir = Get-Location

try {

# -- Locate the repo root -----------------------------------------------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
Set-Location $RepoRoot
# .NET file APIs (WriteAllText etc.) honour [Environment]::CurrentDirectory
# rather than Set-Location; sync it explicitly so the script works from
# any starting directory.
[Environment]::CurrentDirectory = $RepoRoot

# -- Platform check -----------------------------------------------------------
if (-not $IsWindows -and $null -ne $IsWindows) {
    # PowerShell Core on a non-Windows host
    Log-Warn "This is the Windows-flavoured init script; the host OS is not Windows."
    Log-Warn "On Mac/Linux use: ./scripts/init-project.sh"
    $yn = Read-Host "Continue anyway? [y/N]"
    if ($yn -notmatch '^[Yy]$') { exit 1 }
}

Log-Info "Repo     : $RepoRoot"
Log-Info "Host OS  : Windows ($env:PROCESSOR_ARCHITECTURE)"

# -- Probe tools --------------------------------------------------------------
function Find-Cmd($name) {
    # PowerShell in the Git Bash environment needs the .exe suffix for MSYS2 tools.
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    # Try with .exe suffix
    $c = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

$CmakePath = Find-Cmd "cmake"
$GccPath   = Find-Cmd "g++"
$GdbPath   = Find-Cmd "gdb"
$NinjaPath = Find-Cmd "ninja"

# Fallback: if the tool is not on PATH, scan common MSYS2 UCRT64 install dirs.
$Msys2Dirs = @(
    "D:\worksoft\msys64\ucrt64\bin",
    "C:\msys64\ucrt64\bin",
    "D:\msys64\ucrt64\bin",
    "D:\tools\msys64\ucrt64\bin"
)
foreach ($msysDir in $Msys2Dirs) {
    if (-not (Test-Path $msysDir)) { continue }
    if (-not $GccPath)   { $exe = Join-Path $msysDir "g++.exe";   if (Test-Path $exe) { $GccPath   = $exe } }
    if (-not $GdbPath)   { $exe = Join-Path $msysDir "gdb.exe";   if (Test-Path $exe) { $GdbPath   = $exe } }
    if (-not $NinjaPath) { $exe = Join-Path $msysDir "ninja.exe"; if (Test-Path $exe) { $NinjaPath = $exe } }
    if (-not $CmakePath) { $exe = Join-Path $msysDir "cmake.exe"; if (Test-Path $exe) { $CmakePath = $exe } }
}

Log-Info "cmake    : $(if ($CmakePath) { $CmakePath } else { 'not found' })"
Log-Info "g++      : $(if ($GccPath)   { $GccPath }   else { 'not found' })"
Log-Info "gdb      : $(if ($GdbPath)   { $GdbPath }   else { 'not found' })"
Log-Info "ninja    : $(if ($NinjaPath) { $NinjaPath } else { 'not found' })"

# Default Windows toolchain: MSYS2 UCRT64 + MinGW GCC.
$ToolchainKind = $null   # "mingw" or null
if ($GccPath) { $ToolchainKind = "mingw" }

# -- Probe Qt6 ----------------------------------------------------------------
function Find-Qt6 {
    if ($env:ARIA_NO_QT6 -eq "1") { return $null }
    if ($env:QT_DIR) {
        if (Test-Path (Join-Path $env:QT_DIR "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $env:QT_DIR
        }
        Log-Warn "QT_DIR is not a valid Qt6 install root: $($env:QT_DIR)"
    }
    # 1) Standard Qt installer paths
    $roots = @("C:\Qt", "D:\Qt", "$env:USERPROFILE\Qt")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $versions = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^6\.' } |
                    Sort-Object Name -Descending
        foreach ($v in $versions) {
            # Toolchain: MSYS2 UCRT64 -> mingw_64
            $prefOrder = @("mingw_64")
            foreach ($kit in $prefOrder) {
                $p = Join-Path $v.FullName $kit
                if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
                    return $p
                }
            }
        }
    }

    # 2) Qt6 installed via MSYS2 UCRT64 pacman.
    $msys2Candidates = @("C:\msys64\ucrt64", "D:\msys64\ucrt64", "D:\worksoft\msys64\ucrt64")
    foreach ($p in $msys2Candidates) {
        if (Test-Path (Join-Path $p "lib\cmake\Qt6\Qt6Config.cmake")) {
            return $p
        }
    }

    return $null
}

$QtDir = Find-Qt6
if ($QtDir) {
    Log-Ok "Qt6      : $QtDir"
} else {
    Log-Warn "Qt6      : not found"
}

# -- Dependency gate ----------------------------------------------------------
$MissingDeps = @()
if (-not $CmakePath) { $MissingDeps += "cmake" }
if (-not $ToolchainKind) { $MissingDeps += "compiler" }
if (-not $QtDir -and $env:ARIA_NO_QT6 -ne "1") { $MissingDeps += "qt6" }

if ($MissingDeps.Count -gt 0) {
    Write-Host ""
    Log-Err "Missing required dependencies: $($MissingDeps -join ', ')"
    Write-Host "       Skipping .vscode/ generation - install the deps below and rerun."
    Write-Host ""

    # Detect available package managers.
    $HasWinget = $null -ne (Find-Cmd "winget")
    $HasChoco  = $null -ne (Find-Cmd "choco")
    $HasScoop  = $null -ne (Find-Cmd "scoop")

    Write-Host "  > Suggested install paths:" -ForegroundColor Cyan
    foreach ($dep in $MissingDeps) {
        switch ($dep) {
            "cmake" {
                Write-Host "    CMake:"
                if ($HasWinget) { Write-Host "      winget install -e --id Kitware.CMake" }
                elseif ($HasChoco) { Write-Host "      choco install cmake -y" }
                elseif ($HasScoop) { Write-Host "      scoop install cmake" }
                else { Write-Host "      Download: https://cmake.org/download/  (tick 'Add to PATH')" }
            }
            "compiler" {
                Write-Host "    Compiler (MSYS2 + MinGW UCRT64):"
                if ($HasWinget) {
                    Write-Host "           winget install -e --id MSYS2.MSYS2"
                } else {
                    Write-Host "           Download: https://www.msys2.org/"
                }
                Write-Host "         Then in the MSYS2 UCRT64 shell:"
                Write-Host "           pacman -S --needed mingw-w64-ucrt-x86_64-toolchain ``"
                Write-Host "                             mingw-w64-ucrt-x86_64-cmake ``"
                Write-Host "                             mingw-w64-ucrt-x86_64-ninja"
                Write-Host "         Then add C:\msys64\ucrt64\bin to your PATH."
            }
            "qt6" {
                Write-Host "    Qt 6.x (mingw_64 kit):"
                Write-Host "      (1) Online installer: https://www.qt.io/download-qt-installer"
                Write-Host "         Default install path: C:\Qt\6.x.x\mingw_64"
                if ($HasWinget) {
                    Write-Host "      (2) winget (third-party mirrors, partial coverage):"
                    Write-Host "           winget search qt"
                }
                Write-Host "      (3) Already installed at a non-standard path? Set the env var and rerun:"
                Write-Host "           `$env:QT_DIR = 'C:\path\to\qt\6.6.0\mingw_64'"
            }
        }
        Write-Host ""
    }
    Write-Host "  Once everything is in place, rerun:" -ForegroundColor Cyan
    Write-Host "      .\scripts\init-project.ps1"
    Write-Host ""
    Write-Host "  To deliberately skip Qt6 (no demo1 build):" -ForegroundColor Cyan
    Write-Host "      `$env:ARIA_NO_QT6 = '1'; .\scripts\init-project.ps1"
    exit 1
}

# -- Helpers for writing .vscode/ ---------------------------------------------
# Strategy (mirrors init-project.sh):
#   default        -> overwrite
#   -NoOverwrite   -> skip if file already exists
# Files whose contents already match the template are silently no-op'd
# to avoid bumping their mtime needlessly.
function Write-File($dest, $content) {
    if ((Test-Path $dest) -and $NoOverwrite) {
        Log-Dim "skip (exists) : $dest"
        return
    }

    if ($DryRun) {
        $lines = ($content -split "`n").Count
        if (Test-Path $dest) {
            Write-Host "  would update  : $dest ($lines lines)"
        } else {
            Write-Host "  would write   : $dest ($lines lines)"
        }
        return
    }

    $parent = Split-Path $dest -Parent
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }

    $utf8NoBom = New-Object System.Text.UTF8Encoding $false

    if (Test-Path $dest) {
        try {
            $existing = [System.IO.File]::ReadAllText($dest, $utf8NoBom)
        } catch {
            $existing = $null
        }
        if ($null -ne $existing -and $existing -eq $content) {
            Log-Dim "unchanged     : $dest"
            return
        }
        [System.IO.File]::WriteAllText($dest, $content, $utf8NoBom)
        Log-Ok "  updated       : $dest"
        return
    }

    [System.IO.File]::WriteAllText($dest, $content, $utf8NoBom)
    Log-Ok "  wrote         : $dest"
}

# -- Pick generator / IntelliSense mode / debug engine ------------------------
$Generator = if ($NinjaPath) { "Ninja" } else { "MinGW Makefiles" }
$IntelliSense = "windows-gcc-x64"
$DebugType = "cppdbg"       # gdb
$MIMode = "gdb"
$CompilerPath = $GccPath -replace '\\', '\\'

# Backslashes need escaping inside the JSON templates.
$QtDirEsc    = $QtDir    -replace '\\', '\\'
$CmakeEsc    = $CmakePath -replace '\\', '\\'
$GdbEsc      = if ($GdbPath) { $GdbPath -replace '\\', '\\' } else { "" }

# PATH passed to VSCode tasks/launch (mingw: toolchain bin first).
$PathEnv = "`${env:PATH}"
if ($ToolchainKind -eq "mingw") {
    $Msys2BinDir = Split-Path -Parent $GccPath
    $PathEnv = "$Msys2BinDir;$PathEnv"
}
$PathEnvEsc = $PathEnv -replace '\\', '\\'

Log-Info "Generating .vscode/ ..."
Log-Info "  toolchain    : $ToolchainKind"
Log-Info "  generator    : $Generator"
Log-Info "  IntelliSense : $IntelliSense"

# -- settings.json -------------------------------------------------------------
$Settings = @"
{
    "cmake.sourceDirectory": "`${workspaceFolder}",
    "cmake.buildDirectory": "`${workspaceFolder}/build/ide",
    "cmake.generator": "$Generator",
    "cmake.configureArgs": [
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCMAKE_PREFIX_PATH=$QtDirEsc",
        "-DARIA_BUILD_QT6=ON",
        "-DARIA_BUILD_TESTS=ON",
        "-DARIA_BUILD_EXAMPLES=ON"
    ],
    "cmake.parallelJobs": 8,
    "C_Cpp.default.compileCommands": "`${workspaceFolder}/build/ide/compile_commands.json",
    "C_Cpp.default.cppStandard": "c++20",
    "C_Cpp.default.intelliSenseMode": "$IntelliSense",
    "files.associations": {
        "*.hpp": "cpp",
        "*.h": "cpp",
        "*.inl": "cpp"
    }
}
"@

# -- extensions.json -----------------------------------------------------------
$Extensions = @"
{
    "recommendations": [
        "ms-vscode.cpptools",
        "ms-vscode.cmake-tools",
        "llvm-vs-code-extensions.vscode-clangd",
        "twxs.cmake"
    ]
}
"@

# -- tasks.json ----------------------------------------------------------------
$Tasks = @"
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "aria: configure (Debug)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "-S", "`${workspaceFolder}",
                "-B", "`${workspaceFolder}/build/ide",
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_PREFIX_PATH=$QtDirEsc",
                "-DARIA_BUILD_QT6=ON",
                "-DARIA_BUILD_EXAMPLES=ON",
                "-DARIA_BUILD_TESTS=ON",
                "-G", "$Generator"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: build demo1 (Debug)",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/1-qt-showcase/scripts/run.ps1",
                "Debug", "--no-launch"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": ["`$gcc"]
        },
        {
            "label": "aria: build all",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build", "`${workspaceFolder}/build/ide",
                "-j", "8"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "dependsOn": ["aria: configure (Debug)"],
            "problemMatcher": ["`$gcc"]
        },
        {
            "label": "aria: run demo1",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/1-qt-showcase/scripts/run.ps1",
                "Debug"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "dependsOn": ["aria: build demo1 (Debug)"],
            "presentation": { "reveal": "always", "panel": "new" },
            "problemMatcher": []
        },
        {
            "label": "aria: probe demo1",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/1-qt-showcase/scripts/run.ps1",
                "Debug", "probe"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "dependsOn": ["aria: build demo1 (Debug)"],
            "problemMatcher": []
        },
        {
            "label": "aria: build demo4 (Debug, no launch)",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/4-web-mvvm/scripts/run.ps1",
                "Debug", "--no-launch"
            ],
            "problemMatcher": ["`$gcc"]
        },
        {
            "label": "aria: run demo4 (HTTP)",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/4-web-mvvm/scripts/run.ps1",
                "Debug"
            ],
            "presentation": { "reveal": "always", "panel": "new" },
            "problemMatcher": []
        },
        {
            "label": "aria: run demo4 (HTTPS)",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/4-web-mvvm/scripts/run.ps1",
                "Debug", "--tls"
            ],
            "presentation": { "reveal": "always", "panel": "new" },
            "problemMatcher": []
        },
        {
            "label": "aria: probe demo4",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/4-web-mvvm/scripts/run.ps1",
                "Debug", "--probe"
            ],
            "problemMatcher": []
        },
        {
            "label": "aria: build demo5 (Debug, no launch)",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/5-android-jni-mvvm/scripts/run.ps1",
                "Debug", "--no-launch"
            ],
            "group": "build",
            "problemMatcher": ["`$gcc"]
        },
        {
            "label": "aria: run demo5 (Android JNI + Compose)",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-ExecutionPolicy", "Bypass",
                "-File", "`${workspaceFolder}/examples/5-android-jni-mvvm/scripts/run.ps1",
                "Debug"
            ],
            "presentation": { "reveal": "always", "panel": "new" },
            "problemMatcher": []
        },
        {
            "label": "aria: ctest",
            "type": "shell",
            "command": "ctest",
            "args": ["--output-on-failure"],
            "options": {
                "cwd": "`${workspaceFolder}/build/ide",
                "env": { "PATH": "$PathEnvEsc" }
            },
            "dependsOn": ["aria: build all"],
            "problemMatcher": []
        },
        {
            "label": "aria: configure (ASan+UBSan)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "-S", "`${workspaceFolder}",
                "-B", "`${workspaceFolder}/build/flavors/asan",
                "-G", "$Generator",
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_PREFIX_PATH=$QtDirEsc",
                "-DARIA_ENABLE_ASAN=ON",
                "-DARIA_ENABLE_UBSAN=ON",
                "-DARIA_BUILD_TESTS=ON",
                "-DARIA_BUILD_EXAMPLES=OFF"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: ctest (ASan+UBSan)",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-NoProfile",
                "-Command",
                "cmake --build build/flavors/asan -j; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; ctest --test-dir build/flavors/asan --output-on-failure"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "dependsOn": ["aria: configure (ASan+UBSan)"],
            "problemMatcher": []
        },
        {
            "label": "aria: build (Android NDK)",
            "type": "shell",
            "command": "bash",
            "args": [
                "-c",
                "scripts/build.sh android"
            ],
            "options": {
                "cwd": "`${workspaceFolder}"
            },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: clang-tidy",
            "type": "shell",
            "command": "powershell",
            "args": [
                "-NoProfile",
                "-Command",
                "cmake -S . -B build/ide -G '$Generator' -DCMAKE_EXPORT_COMPILE_COMMANDS=ON | Out-Null; cmake --build build/ide -j | Out-Null; `$files = Get-ChildItem -Recurse -Path 'modules/*/include/aria' -Filter *.hpp | ForEach-Object FullName; clang-tidy -p build/ide --warnings-as-errors=\`"*\`" @files"
            ],
            "options": {
                "env": { "PATH": "$PathEnvEsc" }
            },
            "problemMatcher": [
                {
                    "owner": "clang-tidy",
                    "fileLocation": ["autoDetect", "`${workspaceFolder}"],
                    "pattern": {
                        "regexp": "^(.*?):(\\\\d+):(\\\\d+):\\\\s+(warning|error):\\\\s+(.*)$",
                        "file": 1, "line": 2, "column": 3, "severity": 4, "message": 5
                    }
                }
            ]
        }
    ]
}
"@

# -- launch.json ---------------------------------------------------------------
# Demo1 (Qt) Windows: examples/1-qt-showcase/scripts/run.ps1 builds into
# build/examples/1-qt-showcase/bin/ex_qt_showcase.exe (per-demo isolated cache).
$ProgramRel = "`${workspaceFolder}/build/examples/1-qt-showcase/bin/ex_qt_showcase.exe"
$LaunchCwd = "`${workspaceFolder}/build/examples/1-qt-showcase/bin"
# Launch PATH: demo1's bin (DLLs) + MSYS2 (runtime) + system PATH
$LaunchPath = "`${workspaceFolder}\\build\\examples\\1-qt-showcase\\bin;$PathEnvEsc"

$Launch = @"
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Run demo1 (Qt showcase)",
            "type": "node-terminal",
            "request": "launch",
            "command": "powershell -ExecutionPolicy Bypass -File examples\\\\1-qt-showcase\\\\scripts\\\\run.ps1 Debug",
            "cwd": "`${workspaceFolder}"
        },
        {
            "name": "Debug demo1 (Qt showcase, gdb)",
            "type": "cppdbg",
            "request": "launch",
            "program": "$ProgramRel",
            "args": [],
            "stopAtEntry": false,
            "cwd": "$LaunchCwd",
            "environment": [
                { "name": "PATH", "value": "$LaunchPath" }
            ],
            "externalConsole": false,
            "MIMode": "gdb",
            "miDebuggerPath": "$GdbEsc",
            "preLaunchTask": "aria: build demo1 (Debug)"
        },
        {
            "name": "Debug demo1 - probe mode (gdb)",
            "type": "cppdbg",
            "request": "launch",
            "program": "$ProgramRel",
            "args": [],
            "stopAtEntry": false,
            "cwd": "$LaunchCwd",
            "environment": [
                { "name": "ARIA_PROBE", "value": "1" },
                { "name": "PATH", "value": "$LaunchPath" }
            ],
            "externalConsole": false,
            "MIMode": "gdb",
            "miDebuggerPath": "$GdbEsc",
            "preLaunchTask": "aria: build demo1 (Debug)"
        },
        {
            "name": "Run demo4 (Web MVVM, HTTP)",
            "type": "node-terminal",
            "request": "launch",
            "command": "powershell -ExecutionPolicy Bypass -File examples\\\\4-web-mvvm\\\\scripts\\\\run.ps1 Debug",
            "cwd": "`${workspaceFolder}"
        },
        {
            "name": "Run demo4 (Web MVVM, HTTPS)",
            "type": "node-terminal",
            "request": "launch",
            "command": "powershell -ExecutionPolicy Bypass -File examples\\\\4-web-mvvm\\\\scripts\\\\run.ps1 Debug --tls",
            "cwd": "`${workspaceFolder}"
        },
        {
            "name": "Run demo4 - probe mode",
            "type": "node-terminal",
            "request": "launch",
            "command": "powershell -ExecutionPolicy Bypass -File examples\\\\4-web-mvvm\\\\scripts\\\\run.ps1 Debug --probe",
            "cwd": "`${workspaceFolder}"
        },
        {
            "name": "Run demo5 (Android JNI + Compose)",
            "type": "node-terminal",
            "request": "launch",
            "command": "powershell -ExecutionPolicy Bypass -File examples\\\\5-android-jni-mvvm\\\\scripts\\\\run.ps1 Debug",
            "cwd": "`${workspaceFolder}"
        },
        {
            "name": "Debug test (pick binary, gdb)",
            "type": "cppdbg",
            "request": "launch",
            "program": "`${workspaceFolder}/build/ide/bin/test_`${input:testModule}.exe",
            "args": [],
            "stopAtEntry": false,
            "cwd": "$LaunchCwd",
            "environment": [
                { "name": "PATH", "value": "$LaunchPath" }
            ],
            "MIMode": "gdb",
            "miDebuggerPath": "$GdbEsc",
            "preLaunchTask": "aria: build all"
        }
    ],
    "inputs": [
        {
            "id": "testModule",
            "type": "pickString",
            "description": "Which test module?",
            "options": ["abi", "core", "async", "runtime", "binding"],
            "default": "core"
        }
    ]
}
"@

# -- c_cpp_properties.json -----------------------------------------------------
$CCppProperties = @"
{
    "version": 4,
    "configurations": [
        {
            "name": "Aria (CMake - framework & demo1)",
            "compileCommands": "`${workspaceFolder}/build/ide/compile_commands.json",
            "compilerPath": "$CompilerPath",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "$IntelliSense"
        },
        {
            "name": "demo5 (Android JNI / Gradle)",
            "compilerPath": "$CompilerPath",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "$IntelliSense",
            "includePath": [
                "`${workspaceFolder}/modules/adapters/jni/include",
                "`${workspaceFolder}/modules/core/include",
                "`${workspaceFolder}/modules/abi/include",
                "`${workspaceFolder}/modules/binding/include",
                "`${workspaceFolder}/modules/runtime/include",
                "`${workspaceFolder}/modules/async/include"
            ],
            "defines": ["DEBUG=1", "ANDROID=1"]
        }
    ]
}
"@

Write-File ".vscode\settings.json"          $Settings
Write-File ".vscode\extensions.json"        $Extensions
Write-File ".vscode\tasks.json"             $Tasks
Write-File ".vscode\launch.json"            $Launch
Write-File ".vscode\c_cpp_properties.json"  $CCppProperties

if (-not $DryRun) {
    if (-not (Test-Path "build")) { New-Item -ItemType Directory -Path "build" -Force | Out-Null }
    if (-not (Test-Path "build/platforms")) { New-Item -ItemType Directory -Path "build/platforms" -Force | Out-Null }
}

Write-Host ""
Log-Ok "Project initialised."
Write-Host "  Next steps:"
Write-Host "    * Open this folder in VSCode."
Write-Host "    * When prompted, install all recommended extensions."
Write-Host "    * Press F5 -> pick 'Debug demo1 (Qt showcase)' to start a debug session."
Write-Host "    * Or run:  .\examples\1-qt-showcase\scripts\run.ps1  /  .\examples\5-android-jni-mvvm\scripts\run.ps1"

} finally {
    Set-Location $OriginalDir
}
