# examples/5-android-jni-mvvm/scripts/run.ps1 - build & install the Android JNI + Compose demo
#
# 用法：
#   .\examples\5-android-jni-mvvm\scripts\run.ps1                        # Debug, 安装到设备
#   .\examples\5-android-jni-mvvm\scripts\run.ps1 Release                # Release
#   .\examples\5-android-jni-mvvm\scripts\run.ps1 Debug -NoLaunch      # 只构建不安装
#   .\examples\5-android-jni-mvvm\scripts\run.ps1 Debug -Probe          # 构建 → 安装 → 启动 → 验证
#
# 环境变量：
#   ANDROID_SDK_ROOT   Android SDK 路径（默认自动探测）
#   ANDROID_NDK_ROOT   Android NDK 路径（默认自动探测）
#   ARIA_JNI_CMAKE     CMake 可执行文件路径（默认用 SDK 自带的 cmake）

param(
    [string]$BuildType = "Debug",
    [switch]$NoLaunch,
    [switch]$Probe
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DemoRoot  = Resolve-Path (Join-Path $ScriptDir "..")
$RepoRoot  = Resolve-Path (Join-Path $DemoRoot "..\..")

Write-Host "[demo5] Repo root : $RepoRoot" -ForegroundColor Blue
Write-Host "[demo5] Demo root : $DemoRoot" -ForegroundColor Blue
Write-Host "[demo5] Build type: $BuildType" -ForegroundColor Blue

# -- 探测 Android SDK / NDK --------------------------------------------------
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
    Write-Host "[demo5] ANDROID_SDK_ROOT not set and cannot be auto-detected" -ForegroundColor Red
    Write-Host "        Please set ANDROID_SDK_ROOT and try again" -ForegroundColor Red
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
    Write-Host "[demo5] ANDROID_NDK_ROOT not set and cannot be auto-detected" -ForegroundColor Red
    Write-Host "        Please set ANDROID_NDK_ROOT and try again" -ForegroundColor Red
    exit 1
}

Write-Host "[demo5] SDK     : $AndroidSdkRoot" -ForegroundColor Green
Write-Host "[demo5] NDK     : $AndroidNdkRoot" -ForegroundColor Green

# -- CMake（用 SDK 自带的）----------------------------------------------------
$AriaJniCmake = $env:ARIA_JNI_CMAKE
if (-not $AriaJniCmake) {
    $sdkCmakeDir = Join-Path $AndroidSdkRoot "cmake"
    if (Test-Path $sdkCmakeDir) {
        $latestCmake = Get-ChildItem $sdkCmakeDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($latestCmake) {
            $AriaJniCmake = Join-Path $latestCmake.FullName "bin\cmake.exe"
        }
    }
}
if (-not $AriaJniCmake) {
    $AriaJniCmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
}
if (-not $AriaJniCmake) {
    Write-Host "[demo5] CMake not found. Set ARIA_JNI_CMAKE or install CMake" -ForegroundColor Red
    exit 1
}

Write-Host "[demo5] CMake   : $AriaJniCmake" -ForegroundColor Green

# -- Gradle 构建 --------------------------------------------------------------
$GradleCmd = Join-Path $DemoRoot "gradlew.bat"
if (-not (Test-Path $GradleCmd)) {
    Write-Host "[demo5] gradlew.bat not found at: $GradleCmd" -ForegroundColor Red
    Write-Host "        Please create the Android project first" -ForegroundColor Red
    exit 1
}

$GradleTask = if ($BuildType -eq "Release") { "assembleRelease" } else { "assembleDebug" }
Write-Host "[demo5] Gradle task: $GradleTask" -ForegroundColor Blue

$env:ANDROID_SDK_ROOT = $AndroidSdkRoot
$env:ANDROID_NDK_ROOT = $AndroidNdkRoot
$env:ARIA_JNI_CMAKE   = $AriaJniCmake

Push-Location $DemoRoot
try {
    & $GradleCmd $GradleTask "-Paria.repo.root=$RepoRoot"
    Write-Host "[demo5] Gradle build complete" -ForegroundColor Green

    if ($NoLaunch) {
        Write-Host "[demo5] -NoLaunch specified, skipping install" -ForegroundColor Yellow
        exit 0
    }

    # -- 安装到设备 ------------------------------------------------------------
    $ApkDir = if ($BuildType -eq "Release") {
        Join-Path $DemoRoot "app\build\outputs\apk\release"
    } else {
        Join-Path $DemoRoot "app\build\outputs\apk\debug"
    }

    $ApkFile = Get-ChildItem $ApkDir -Filter "*.apk" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $ApkFile) {
        Write-Host "[demo5] APK file not found in $ApkDir" -ForegroundColor Red
        exit 1
    }

    Write-Host "[demo5] APK     : $($ApkFile.FullName)" -ForegroundColor Blue

    $adb = Get-Command adb -ErrorAction SilentlyContinue
    if (-not $adb) {
        Write-Host "[demo5] adb not found. Add Android SDK platform-tools to PATH" -ForegroundColor Red
        exit 1
    }

    Write-Host "[demo5] Installing APK to device..."
    & adb install -r $ApkFile.FullName
    Write-Host "[demo5] Install complete" -ForegroundColor Green

    # -- 启动应用 -------------------------------------------------------------
    $PackageName = "com.example.aria.demo5"
    $ActivityName = ".MainActivity"

    Write-Host "[demo5] Launching: $ActivityName"
    & adb shell am start -n $ActivityName

    if ($Probe) {
        Write-Host "[demo5] Probe mode: verifying app is running..."
        Start-Sleep -Seconds 2
        $dumpsys = & adb shell dumpsys window windows 2>$null
        if ($dumpsys -match $PackageName) {
            Write-Host "[demo5] App is running in foreground" -ForegroundColor Green
        } else {
            Write-Host "[demo5] App may not have started successfully" -ForegroundColor Red
            exit 1
        }
    }

    Write-Host "[demo5] Done" -ForegroundColor Green
} finally {
    Pop-Location
}
