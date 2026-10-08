#!/usr/bin/env python3
"""One portable CMake entry point; dependency recipes remain in their existing tools.

Use --dry-run to inspect without fetching, configuring or writing build trees.
Mobile targets build native libraries/apps, not APK packaging or device deployment.
Existing packaging and deployment scripts remain available for those operations.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def project(root):
    if (root / "Workbench/CMakeLists.txt").is_file():
        return "tools"
    if (root / "core/agent").is_dir():
        return "agent"
    if (root / "demos").is_dir():
        return "tutorial"
    if (root / "modules/abi").is_dir():
        return "aria"
    raise ValueError("Unsupported repository layout")


def positive(value):
    number = int(value)
    if not 1 <= number <= 256:
        raise argparse.ArgumentTypeError("--jobs must be between 1 and 256")
    return number


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", choices=("build", "deps"), default="build")
    parser.add_argument("--platform", choices=("native", "qt", "web", "ios", "android"), default="native")
    parser.add_argument("--toolchain", choices=("auto", "msvc", "mingw"), default="auto")
    parser.add_argument("--config", choices=("Release", "Debug", "RelWithDebInfo", "MinSizeRel"), default="Release")
    parser.add_argument("--jobs", type=positive, default=min(os.cpu_count() or 1, 4))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--generator")
    parser.add_argument("--aria-root", type=Path, help="Explicit local Aria source (consumers only)")
    parser.add_argument("--qt-prefix", type=Path, default=os.environ.get("QT_DIR"))
    parser.add_argument("--ndk", type=Path, default=os.environ.get("ANDROID_NDK_ROOT"))
    parser.add_argument("--arch", help="Android ABI or Apple architecture")
    parser.add_argument("--ios-sdk", choices=("iphonesimulator", "iphoneos"), default="iphonesimulator")
    parser.add_argument("--test", action="store_true", help="Run native-host CTest; mobile needs a separate device runner")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--cmake-arg", action="append", default=[], help="Extra argument, e.g. --cmake-arg=-DNAME=VALUE")
    return parser.parse_args(argv)


def plan(args, root=ROOT, host=None):
    host = host or platform.system()
    kind = project(root)
    target = args.platform
    supported = {"aria": {"native", "qt", "web", "ios", "android"},
                 "tools": {"native", "qt", "web", "ios", "android"},
                 "agent": {"native", "qt"}, "tutorial": {"native", "qt", "web"}}
    if target not in supported[kind]:
        raise ValueError(f"{kind}: {target} is not implemented in this repository")
    if target == "ios" and host != "Darwin":
        raise ValueError("iOS requires macOS and Xcode")
    if args.test and target in {"ios", "android"}:
        raise ValueError("Mobile execution requires the existing simulator/device runner; do not run host CTest")
    toolchain = ("msvc" if host == "Windows" else "native") if args.toolchain == "auto" else args.toolchain
    if host != "Windows" and toolchain != "native":
        raise ValueError("--toolchain msvc/mingw is only valid on Windows")
    arch = args.arch or ("arm64-v8a" if target == "android" else platform.machine())
    suffix = f"{target}-{toolchain}-{args.config.lower()}-{arch}"
    if target == "ios":
        suffix += "-" + args.ios_sdk
    build = (args.build_dir or root / "build/unified" / suffix).resolve()
    source = root / "Workbench" if kind == "tools" else root
    commands = []
    aria = args.aria_root.resolve() if args.aria_root else root / "build/deps/aria"
    if kind != "aria" and not args.aria_root:
        fetch = [sys.executable, str(root / "tools/ci/fetch_aria.py")]
        if args.offline:
            fetch.append("--offline")
        commands.append(fetch)
    if args.command == "deps":
        if kind == "aria":
            commands.append([sys.executable, str(root / "scripts/dependencies.py"), "resolve",
                             "--file", str(root / "dependencies.json")] + (["--offline"] if args.offline else []))
        return commands
    if args.aria_root and kind == "aria":
        raise ValueError("--aria-root applies only to consuming projects")
    flags = [f"-DCMAKE_BUILD_TYPE={args.config}", f"-DARIA_DEPENDENCIES_OFFLINE={'ON' if args.offline else 'OFF'}"]
    if kind == "aria":
        flags += [f"-DARIA_BUILD_TESTS={'ON' if args.test else 'OFF'}",
                  "-DARIA_BUILD_BENCHMARK=OFF", "-DARIA_BUILD_DOCS=OFF",
                  f"-DARIA_BUILD_QT6={'ON' if target == 'qt' else 'OFF'}",
                  f"-DARIA_BUILD_HTTP={'ON' if target == 'web' else 'OFF'}",
                  f"-DARIA_BUILD_UIKIT={'ON' if target == 'ios' else 'OFF'}",
                  f"-DARIA_BUILD_JNI={'ON' if target == 'android' else 'OFF'}"]
    elif kind == "tools":
        selected = "qt" if target == "native" else target
        flags += [f"-DWORKBENCH_TARGET_{name.upper()}={'ON' if name == selected else 'OFF'}"
                  for name in ("qt", "web", "ios", "android")]
        flags += [f"-DARIA_DIR={aria}"]
    elif kind == "agent":
        flags += [f"-DARIA_DIR={aria}", "-DARIA_TARGET_QT=ON"]
    else:
        flags += [f"-DARIA_ROOT={aria}", f"-DBUILD_TESTING={'ON' if args.test else 'OFF'}",
                  f"-DARIA_TUTORIAL_QT6={'ON' if target == 'qt' else 'OFF'}",
                  f"-DARIA_TUTORIAL_HTTP={'ON' if target == 'web' else 'OFF'}"]
    qt_prefix = args.qt_prefix
    needs_qt = target == "qt" or (target == "native" and kind in {"tools", "agent"})
    if not qt_prefix and needs_qt and host == "Darwin" and not args.dry_run and shutil.which("brew"):
        found = subprocess.run(["brew", "--prefix", "qt"], capture_output=True, text=True,
                               encoding="utf-8", check=False)
        if found.returncode == 0:
            qt_prefix = Path(found.stdout.strip())
    if qt_prefix:
        flags.append(f"-DCMAKE_PREFIX_PATH={qt_prefix}")
    generator = args.generator
    if target == "ios":
        generator = generator or "Xcode"
        flags += ["-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_SYSROOT={args.ios_sdk}",
                  f"-DCMAKE_OSX_ARCHITECTURES={args.arch or 'arm64'}", "-DARIA_BUILD_SHARED=OFF"]
    if target == "android":
        if not args.ndk:
            raise ValueError("Android requires --ndk or ANDROID_NDK_ROOT")
        flags += [f"-DCMAKE_TOOLCHAIN_FILE={args.ndk}/build/cmake/android.toolchain.cmake",
                  f"-DANDROID_ABI={arch}", "-DANDROID_PLATFORM=android-24", "-DARIA_BUILD_SHARED=OFF"]
        generator = generator or "Ninja"
    if toolchain == "mingw":
        generator = generator or "Ninja"
        flags += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"]
    # On Windows CMake's default Visual Studio generator discovers MSVC;
    # a caller choosing Ninja must use a configured developer environment.
    configure = ["cmake", "-S", str(source), "-B", str(build)]
    if generator:
        configure += ["-G", generator]
    commands.append(configure + flags + args.cmake_arg)
    if args.configure_only:
        return commands
    commands.append(["cmake", "--build", str(build), "--config", args.config,
                     "--parallel", str(args.jobs)] + (["--target", *args.target] if args.target else []))
    if args.test and kind == "tools":
        # The application tree has no CTest suite: its six independent module
        # projects must each configure, build and run. Do not silently omit one.
        for module in ("calendar", "cart", "dashboard", "frameworklab", "notes", "tools"):
            module_build = build / "module-tests" / module
            configure_module = ["cmake", "-S", str(source / "modules" / module / "tests"),
                                "-B", str(module_build)]
            if generator:
                configure_module += ["-G", generator]
            compiler_flags = (["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"]
                              if toolchain == "mingw" else [])
            commands.append(configure_module + compiler_flags + args.cmake_arg + [
                f"-DCMAKE_BUILD_TYPE={args.config}", f"-DARIA_DIR={aria}",
                f"-DARIA_DEPS_CACHE_DIR={source / 'build/_deps'}",
                f"-DARIA_DEPENDENCIES_OFFLINE={'ON' if args.offline else 'OFF'}",
                "-DWORKBENCH_TARGET_QT=OFF", "-DWORKBENCH_TARGET_IOS=OFF"])
            commands.append(["cmake", "--build", str(module_build), "--config", args.config,
                             "--parallel", str(args.jobs)])
            commands.append(["ctest", "--test-dir", str(module_build), "-C", args.config,
                             "--output-on-failure", "--no-tests=error"])
    elif args.test:
        commands.append(["ctest", "--test-dir", str(build), "-C", args.config,
                         "--output-on-failure", "--no-tests=error"])
    return commands


def validate_cache(command):
    """Reject conflicting explicit cache reuse, never delete or relabel it."""
    if command[0] != "cmake" or "-S" not in command or "-B" not in command:
        return
    directory = Path(command[command.index("-B") + 1])
    cache = directory / "CMakeCache.txt"
    if not cache.is_file():
        return
    values = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    expected = {"CMAKE_HOME_DIRECTORY": str(Path(command[command.index("-S") + 1]).resolve())}
    if "-G" in command:
        expected["CMAKE_GENERATOR"] = command[command.index("-G") + 1]
    for argument in command:
        if argument.startswith("-D") and "=" in argument:
            key, value = argument[2:].split("=", 1)
            if key in {"CMAKE_BUILD_TYPE", "CMAKE_TOOLCHAIN_FILE", "ANDROID_ABI", "CMAKE_OSX_SYSROOT"}:
                expected[key] = value
    for key, value in expected.items():
        actual = values.get(key)
        if actual and actual != value:
            if key in {"CMAKE_HOME_DIRECTORY", "CMAKE_TOOLCHAIN_FILE"} and Path(actual).resolve() == Path(value).resolve():
                continue
            raise ValueError(f"{key} conflicts with cache {directory}: {actual!r} != {value!r}; choose another --build-dir")


def command_environment(command, env):
    result = dict(env)
    if command[0] == "ctest":
        directory = Path(command[command.index("--test-dir") + 1])
        config = command[command.index("-C") + 1]
        # Standalone module tests and MSVC multi-config executables need the
        # matching runtime directory, not DLLs from another build's PATH.
        result["PATH"] = os.pathsep.join([str(directory / "bin" / config),
                                         str(directory / "bin"), env.get("PATH", "")])
    return result


def main(argv=None):
    args = arguments(argv)
    try:
        commands = plan(args)
        if args.dry_run:
            print(json.dumps(commands, ensure_ascii=False, indent=2))
            return 0
        env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
        if os.name == "nt":
            env["VSLANG"] = "1033"
            if args.toolchain == "mingw":
                prefix = Path(env.get("MSYS2_ROOT", "C:/msys64")) / "ucrt64/bin"
                if not (prefix / "g++.exe").is_file():
                    raise ValueError("MinGW requires MSYS2_ROOT with ucrt64/bin/g++.exe")
                env["PATH"] = str(prefix) + os.pathsep + env.get("PATH", "")
        # Validate every existing cache before fetching dependencies or writing.
        for command in commands:
            validate_cache(command)
        for command in commands:
            print("+ " + shlex.join(command), flush=True)
            subprocess.run(command, cwd=ROOT, env=command_environment(command, env), check=True)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"build: {error}", file=sys.stderr)
        return error.returncode if isinstance(error, subprocess.CalledProcessError) else 2


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    raise SystemExit(main())
