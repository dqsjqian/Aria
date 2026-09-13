#!/usr/bin/env python3
"""Run an Android build's native CTest executables on one adb device.

Usage: python3 scripts/run-android-tests.py build/platforms/android-x86_64
       --serial emulator-5554 --output build/android-test-results.json

Build first. Test commands are discovered from CTest, so a missing executable
fails the run instead of silently reducing coverage. Native tests must be
self-contained executables in the build's bin directory. The JavaScript SDK
test runs on the host in CI; JNI tests here use the native function-table
fixture, not Android UI instrumentation or a live ART application.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time
import uuid


def capture(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True,
                          text=True, **kwargs).stdout.strip()


def cache_values(build):
    result = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("//", "#")) and "=" in line:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"))
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--tests", help="CTest test-name regular expression")
    parser.add_argument("--output", type=Path, help="Write a JSON result summary")
    parser.add_argument("--timeout", type=int, default=180,
                        help="Default timeout per test in seconds (default: 180)")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    build = args.build.resolve()
    values = cache_values(build)
    abi = values.get("ANDROID_ABI", values.get("CMAKE_ANDROID_ARCH_ABI"))
    triples = {"arm64-v8a": "aarch64-linux-android",
               "armeabi-v7a": "arm-linux-androideabi",
               "x86_64": "x86_64-linux-android", "x86": "i686-linux-android"}
    if abi not in triples:
        raise RuntimeError("Build must use an Android NDK toolchain and ABI")
    ndk = Path(values["CMAKE_TOOLCHAIN_FILE"]).resolve().parents[2]
    runtimes = list((ndk / "toolchains/llvm/prebuilt").glob(
        f"*/sysroot/usr/lib/{triples[abi]}/libc++_shared.so"))
    if len(runtimes) != 1:
        raise RuntimeError(f"Expected one NDK libc++ runtime for {abi}, found {runtimes}")

    command = ["ctest", "--test-dir", str(build), "--show-only=json-v1"]
    if args.tests:
        command += ["-R", args.tests]
    manifest = json.loads(capture(command))
    tests = []
    binaries = {}
    for test in manifest["tests"]:
        if test["name"] == "http_web_sdk":
            print("HOST-ONLY http_web_sdk (run separately with Node)", flush=True)
            continue
        command = test.get("command")
        if not command:
            raise RuntimeError(f"{test['name']}: executable is missing; build all test targets first")
        binary = Path(command[0]).resolve()
        if binary.parent != build / "bin" or not binary.is_file():
            raise RuntimeError(f"{test['name']}: expected a native executable in {build / 'bin'}")
        with binary.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                raise RuntimeError(f"{test['name']}: {binary} is not an Android ELF executable")
        binaries[binary.name] = binary
        tests.append(test)
    if not tests:
        raise RuntimeError("No native CTest tests selected")

    adb = [args.adb]
    if args.serial:
        adb += ["-s", args.serial]
    serial = capture(adb + ["get-serialno"], timeout=30)
    # Pin selection even when the user omitted --serial, so a newly connected
    # device cannot change the target halfway through the run.
    adb = [args.adb, "-s", serial]
    device_abis = capture(adb + ["shell", "getprop ro.product.cpu.abilist"], timeout=30)
    if abi not in device_abis.split(","):
        raise RuntimeError(f"Device {serial} supports {device_abis}, build requires {abi}")
    capture(adb + ["shell", "command -v timeout"], timeout=30)

    remote = "/data/local/tmp/aria-tests-" + uuid.uuid4().hex
    files = {path.name: path for path in (build / "bin").glob("*.so*") if path.is_file()}
    files.update(binaries)
    files["libc++_shared.so"] = runtimes[0]
    results = []
    try:
        subprocess.run(adb + ["shell", "mkdir", remote], check=True, timeout=30)
        subprocess.run(adb + ["push", *map(str, files.values()), remote + "/"],
                       check=True, timeout=120)
        subprocess.run(adb + ["shell", "chmod 755 " + " ".join(
            shlex.quote(remote + "/" + name) for name in binaries)], check=True, timeout=30)
        for test in tests:
            properties = {item["name"]: item["value"] for item in test.get("properties", [])}
            timeout = float(properties.get("TIMEOUT", 0)) or args.timeout
            environment = ["LD_LIBRARY_PATH=.", "TMPDIR=" + remote]
            environment += properties.get("ENVIRONMENT", [])
            environment += [f"{name}={os.environ[name]}" for name in
                            ("ARIA_FUZZ_ITERS", "ARIA_FUZZ_SEED") if name in os.environ]
            command = ["timeout", "-s", "KILL", str(timeout), "env", *environment,
                       "./" + Path(test["command"][0]).name, *test["command"][1:]]
            print(f"RUN {test['name']} ({serial}, {abi})", flush=True)
            start = time.monotonic()
            try:
                result = subprocess.run(adb + ["shell", "cd " + shlex.quote(remote) +
                                               " && " + shlex.join(command)], timeout=timeout + 15)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = 124
            results.append({"name": test["name"], "exit_code": status,
                            "seconds": round(time.monotonic() - start, 3)})
            print(f"{'PASS' if status == 0 else 'FAIL'} {test['name']}", flush=True)
    finally:
        subprocess.run(adb + ["shell", "rm", "-rf", remote], check=False, timeout=30)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps({"device": serial, "abi": abi,
                                               "build": str(build),
                                               "selected_tests": [test["name"] for test in tests],
                                               "complete": len(results) == len(tests),
                                               "tests": results}, indent=2) + "\n")
    failed = sum(result["exit_code"] != 0 for result in results)
    print(f"Android native CTest: {len(results) - failed}/{len(results)} passed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
