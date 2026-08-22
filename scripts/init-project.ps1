# init-project.ps1 - bootstrap a freshly-cloned Aria checkout for Windows.
# Generates machine-local VSCode configuration for framework development,
# tests, documentation, benchmarks, sanitizers, and platform builds.

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

function Log-Info { Write-Host "[init] $args" -ForegroundColor Blue }
function Log-Ok   { Write-Host "[ ok] $args" -ForegroundColor Green }
function Log-Warn { Write-Host "[ ! ] $args" -ForegroundColor Yellow }
function Log-Err  { Write-Host "[err] $args" -ForegroundColor Red }

$OriginalDir = Get-Location
try {
    $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $RepoRoot = Split-Path -Parent $ScriptDir
    Set-Location $RepoRoot
    [Environment]::CurrentDirectory = $RepoRoot

    if (-not $IsWindows -and $null -ne $IsWindows) {
        Log-Err "This script targets Windows. On macOS/Linux run ./scripts/init-project.sh"
        exit 1
    }

    function Find-Cmd($Name) {
        $Command = Get-Command $Name -ErrorAction SilentlyContinue
        if ($Command) { return $Command.Source }
        $Command = Get-Command "$Name.exe" -ErrorAction SilentlyContinue
        if ($Command) { return $Command.Source }
        return $null
    }

    $CmakePath = Find-Cmd "cmake"
    $GccPath = Find-Cmd "g++"
    $GdbPath = Find-Cmd "gdb"
    $NinjaPath = Find-Cmd "ninja"
    foreach ($MsysDir in @("D:\worksoft\msys64\ucrt64\bin", "C:\msys64\ucrt64\bin", "D:\msys64\ucrt64\bin")) {
        if (-not (Test-Path $MsysDir)) { continue }
        if (-not $GccPath) { $Path = Join-Path $MsysDir "g++.exe"; if (Test-Path $Path) { $GccPath = $Path } }
        if (-not $GdbPath) { $Path = Join-Path $MsysDir "gdb.exe"; if (Test-Path $Path) { $GdbPath = $Path } }
        if (-not $NinjaPath) { $Path = Join-Path $MsysDir "ninja.exe"; if (Test-Path $Path) { $NinjaPath = $Path } }
        if (-not $CmakePath) { $Path = Join-Path $MsysDir "cmake.exe"; if (Test-Path $Path) { $CmakePath = $Path } }
    }

    if (-not $CmakePath) { Log-Err "cmake not found"; exit 1 }
    if (-not $GccPath) { Log-Err "g++ not found; install MSYS2 UCRT64"; exit 1 }

    function Find-Qt6 {
        if ($env:ARIA_NO_QT6 -eq "1") { return $null }
        if ($env:QT_DIR -and (Test-Path (Join-Path $env:QT_DIR "lib\cmake\Qt6\Qt6Config.cmake"))) {
            return $env:QT_DIR
        }
        foreach ($Root in @("C:\Qt", "D:\Qt", "$env:USERPROFILE\Qt")) {
            if (-not (Test-Path $Root)) { continue }
            $Versions = Get-ChildItem $Root -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^6\.' } | Sort-Object Name -Descending
            foreach ($Version in $Versions) {
                $Candidate = Join-Path $Version.FullName "mingw_64"
                if (Test-Path (Join-Path $Candidate "lib\cmake\Qt6\Qt6Config.cmake")) { return $Candidate }
            }
        }
        foreach ($Candidate in @("C:\msys64\ucrt64", "D:\msys64\ucrt64", "D:\worksoft\msys64\ucrt64")) {
            if (Test-Path (Join-Path $Candidate "lib\cmake\Qt6\Qt6Config.cmake")) { return $Candidate }
        }
        return $null
    }

    $QtDir = Find-Qt6
    $Generator = if ($NinjaPath) { "Ninja" } else { "MinGW Makefiles" }
    $Msys2BinDir = Split-Path -Parent $GccPath
    $PathEnv = "$Msys2BinDir;`${env:PATH}"
    $QtDirEsc = if ($QtDir) { $QtDir -replace '\\', '\\' } else { "" }
    $CompilerEsc = $GccPath -replace '\\', '\\'
    $GdbEsc = if ($GdbPath) { $GdbPath -replace '\\', '\\' } else { "" }
    $PathEnvEsc = $PathEnv -replace '\\', '\\'

    Log-Info "Repo      : $RepoRoot"
    Log-Info "cmake     : $CmakePath"
    Log-Info "compiler  : $GccPath"
    Log-Info "generator : $Generator"
    if ($QtDir) { Log-Ok "Qt6       : $QtDir" } else { Log-Warn "Qt6 adapter disabled" }

    function Write-File($Dest, $Content) {
        if ((Test-Path $Dest) -and $NoOverwrite) { return }
        if ($DryRun) { Write-Host "  would write: $Dest"; return }
        $Parent = Split-Path $Dest -Parent
        if (-not (Test-Path $Parent)) { New-Item -ItemType Directory -Path $Parent -Force | Out-Null }
        $Utf8NoBom = New-Object System.Text.UTF8Encoding $false
        if ((Test-Path $Dest) -and [System.IO.File]::ReadAllText($Dest, $Utf8NoBom) -eq $Content) { return }
        [System.IO.File]::WriteAllText($Dest, $Content, $Utf8NoBom)
        Log-Ok "wrote $Dest"
    }

    if ($QtDir) {
        $QtSettingsArgs = "        `"-DCMAKE_PREFIX_PATH=$QtDirEsc`",`n        `"-DARIA_BUILD_QT6=ON`","
        $QtTaskArgs = "                `"-DCMAKE_PREFIX_PATH=$QtDirEsc`",`n                `"-DARIA_BUILD_QT6=ON`","
    } else {
        $QtSettingsArgs = '        "-DARIA_BUILD_QT6=OFF",'
        $QtTaskArgs = '                "-DARIA_BUILD_QT6=OFF",'
    }

    $Settings = @"
{
    "cmake.sourceDirectory": "`${workspaceFolder}",
    "cmake.buildDirectory": "`${workspaceFolder}/build/ide",
    "cmake.generator": "$Generator",
    "cmake.configureArgs": [
        "-DCMAKE_BUILD_TYPE=Debug",
$QtSettingsArgs
        "-DARIA_BUILD_TESTS=ON",
        "-DARIA_BUILD_DOCS=ON"
    ],
    "cmake.parallelJobs": 8,
    "C_Cpp.default.compileCommands": "`${workspaceFolder}/build/ide/compile_commands.json",
    "C_Cpp.default.cppStandard": "c++20"
}
"@

    $Extensions = @'
{
    "recommendations": [
        "ms-vscode.cpptools",
        "ms-vscode.cmake-tools",
        "llvm-vs-code-extensions.vscode-clangd",
        "twxs.cmake"
    ]
}
'@

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
$QtTaskArgs
                "-DARIA_BUILD_TESTS=ON",
                "-DARIA_BUILD_DOCS=ON",
                "-G", "$Generator"
            ],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: build all",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "`${workspaceFolder}/build/ide", "-j", "8"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "dependsOn": ["aria: configure (Debug)"],
            "group": { "kind": "build", "isDefault": true },
            "problemMatcher": ["`$gcc"]
        },
        {
            "label": "aria: ctest",
            "type": "shell",
            "command": "ctest",
            "args": ["--test-dir", "`${workspaceFolder}/build/ide", "--output-on-failure"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "dependsOn": ["aria: build all"],
            "group": "test",
            "problemMatcher": []
        },
        {
            "label": "aria: configure (ASan+UBSan)",
            "type": "shell",
            "command": "cmake",
            "args": ["-S", "`${workspaceFolder}", "-B", "`${workspaceFolder}/build/flavors/asan", "-G", "$Generator", "-DCMAKE_BUILD_TYPE=Debug", "-DARIA_ENABLE_ASAN=ON", "-DARIA_ENABLE_UBSAN=ON", "-DARIA_BUILD_TESTS=ON"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: ctest (ASan+UBSan)",
            "type": "shell",
            "command": "powershell",
            "args": ["-NoProfile", "-Command", "cmake --build build/flavors/asan -j; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; ctest --test-dir build/flavors/asan --output-on-failure"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "dependsOn": ["aria: configure (ASan+UBSan)"],
            "problemMatcher": []
        },
        {
            "label": "aria: configure (TSan)",
            "type": "shell",
            "command": "cmake",
            "args": ["-S", "`${workspaceFolder}", "-B", "`${workspaceFolder}/build/flavors/tsan", "-G", "$Generator", "-DCMAKE_BUILD_TYPE=Debug", "-DARIA_ENABLE_TSAN=ON", "-DARIA_BUILD_TESTS=ON"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: ctest (TSan)",
            "type": "shell",
            "command": "powershell",
            "args": ["-NoProfile", "-Command", "cmake --build build/flavors/tsan -j; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; `$env:TSAN_OPTIONS='halt_on_error=1'; ctest --test-dir build/flavors/tsan --output-on-failure --timeout 600"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "dependsOn": ["aria: configure (TSan)"],
            "problemMatcher": []
        },
        {
            "label": "aria: docs",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "`${workspaceFolder}/build/ide", "--target", "aria_docs"],
            "dependsOn": ["aria: configure (Debug)"],
            "problemMatcher": []
        },
        {
            "label": "aria: benchmark",
            "type": "shell",
            "command": "powershell",
            "args": ["-NoProfile", "-Command", "cmake -S . -B build/flavors/bench -DCMAKE_BUILD_TYPE=Release -DARIA_BUILD_BENCHMARK=ON; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; cmake --build build/flavors/bench -j"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: build (Android NDK)",
            "type": "shell",
            "command": "powershell",
            "args": ["-ExecutionPolicy", "Bypass", "-File", "`${workspaceFolder}/scripts/build.ps1", "android"],
            "group": "build",
            "problemMatcher": []
        },
        {
            "label": "aria: clang-tidy",
            "type": "shell",
            "command": "powershell",
            "args": ["-NoProfile", "-Command", "cmake -S . -B build/ide -G '$Generator' -DCMAKE_EXPORT_COMPILE_COMMANDS=ON | Out-Null; cmake --build build/ide -j | Out-Null; `$files = Get-ChildItem -Recurse -Path 'modules/*/include/aria' -Filter *.hpp | ForEach-Object FullName; clang-tidy -p build/ide --warnings-as-errors=\`"*\`" @files"],
            "options": { "env": { "PATH": "$PathEnvEsc" } },
            "problemMatcher": []
        }
    ]
}
"@

    $Launch = @"
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug test (pick binary, gdb)",
            "type": "cppdbg",
            "request": "launch",
            "program": "`${workspaceFolder}/build/ide/bin/test_`${input:testModule}.exe",
            "args": [],
            "stopAtEntry": false,
            "cwd": "`${workspaceFolder}",
            "environment": [{ "name": "PATH", "value": "$PathEnvEsc" }],
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

    $CCppProperties = @"
{
    "version": 4,
    "configurations": [
        {
            "name": "Aria framework",
            "compileCommands": "`${workspaceFolder}/build/ide/compile_commands.json",
            "compilerPath": "$CompilerEsc",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "windows-gcc-x64",
            "includePath": ["`${workspaceFolder}/modules/**"]
        }
    ]
}
"@

    Log-Info "Generating .vscode/ ..."
    Write-File ".vscode\settings.json" $Settings
    Write-File ".vscode\extensions.json" $Extensions
    Write-File ".vscode\tasks.json" $Tasks
    Write-File ".vscode\launch.json" $Launch
    Write-File ".vscode\c_cpp_properties.json" $CCppProperties

    if (-not $DryRun) { New-Item -ItemType Directory -Path "build\platforms" -Force | Out-Null }
    Log-Ok "Project initialized. Run 'aria: build all' from VSCode."
    Write-Host "  The flagship sample application is AriaTools."
} finally {
    Set-Location $OriginalDir
}
