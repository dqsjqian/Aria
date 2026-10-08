"""Offline contracts for the portable build entry; no toolchains or network needed."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "scripts/build.py"
if not ENTRY.exists():
    ENTRY = ROOT / "tools/build.py"

spec = importlib.util.spec_from_file_location("build_entry", ENTRY)
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildEntryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="build-entry-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "project with spaces"
        self.root.mkdir()

    def layout(self, kind):
        markers = {"aria": "modules/abi", "tools": "Workbench", "agent": "core/agent", "tutorial": "demos"}
        folder = self.root / markers[kind]
        folder.mkdir(parents=True)
        if kind == "tools":
            (folder / "CMakeLists.txt").write_text("", encoding="utf-8")

    def plan(self, *args, host="Linux"):
        return build.plan(build.arguments(list(args)), self.root, host)

    def test_aria_native_uses_cmake_and_ctest_without_fetch(self):
        self.layout("aria")
        commands = self.plan("--test", "--jobs", "2")
        self.assertEqual([row[0] for row in commands], ["cmake", "cmake", "ctest"])
        self.assertIn("-DARIA_BUILD_TESTS=ON", commands[0])
        self.assertIn("--no-tests=error", commands[-1])
        self.assertIn(str(self.root), commands[0])

    def test_consumers_reuse_existing_locked_fetch(self):
        self.layout("agent")
        commands = self.plan("--offline")
        self.assertEqual(commands[0][0], sys.executable)
        self.assertIn("--offline", commands[0])
        self.assertIn("fetch_aria.py", commands[0][1])
        self.assertIn("-DARIA_DEPENDENCIES_OFFLINE=ON", commands[1])

    def test_local_source_is_explicit_and_not_fetched(self):
        self.layout("tutorial")
        commands = self.plan("--aria-root", str(self.root / "local"), "--test")
        self.assertEqual(commands[0][0], "cmake")
        self.assertIn(f"-DARIA_ROOT={(self.root / 'local').resolve()}", commands[0])

    def test_agent_rejects_unimplemented_mobile_shell(self):
        self.layout("agent")
        with self.assertRaisesRegex(ValueError, "not implemented"):
            self.plan("--platform", "android")

    def test_tools_selects_one_shell(self):
        self.layout("tools")
        configure = self.plan("--platform", "web")[1]
        self.assertIn("-DWORKBENCH_TARGET_WEB=ON", configure)
        self.assertIn("-DWORKBENCH_TARGET_QT=OFF", configure)
        self.assertIn(str(self.root / "Workbench"), configure)

    def test_android_requires_explicit_toolchain(self):
        self.layout("aria")
        with mock.patch.dict(build.os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, "--ndk"):
                self.plan("--platform", "android")
        commands = self.plan("--platform", "android", "--ndk", str(self.root / "NDK"), "--arch", "x86_64")
        self.assertIn("-DANDROID_ABI=x86_64", commands[0])

    def test_mobile_tests_do_not_silently_run_host_ctest(self):
        self.layout("aria")
        with self.assertRaisesRegex(ValueError, "device runner"):
            self.plan("--platform", "ios", "--test", host="Darwin")

    def test_windows_toolchains_have_distinct_caches(self):
        self.layout("aria")
        msvc = self.plan("--toolchain", "msvc", host="Windows")[0]
        mingw = self.plan("--toolchain", "mingw", host="Windows")[0]
        self.assertNotEqual(msvc[msvc.index("-B") + 1], mingw[mingw.index("-B") + 1])
        self.assertIn("-DCMAKE_CXX_COMPILER=g++", mingw)

    def test_ios_sdk_has_distinct_cache(self):
        self.layout("aria")
        simulator = self.plan("--platform", "ios", host="Darwin")[0]
        device = self.plan("--platform", "ios", "--ios-sdk", "iphoneos", host="Darwin")[0]
        self.assertNotEqual(simulator[4], device[4])
        self.assertIn("-DCMAKE_OSX_SYSROOT=iphoneos", device)

    def test_configure_only_does_not_build(self):
        self.layout("aria")
        commands = self.plan("--configure-only", "--cmake-arg=-DARIA_BUILD_HTTP=ON")
        self.assertEqual(len(commands), 1)
        self.assertIn("-DARIA_BUILD_HTTP=ON", commands[0])

    def test_plan_does_not_create_build_tree(self):
        self.layout("aria")
        self.plan("--dry-run")
        self.assertFalse((self.root / "build").exists())

    def test_tools_test_builds_all_six_module_projects(self):
        self.layout("tools")
        commands = self.plan("--platform", "web", "--test", "--offline")
        tests = [row for row in commands if row[0] == "ctest"]
        self.assertEqual(len(tests), 6)
        self.assertEqual([Path(row[2]).name for row in tests],
                         ["calendar", "cart", "dashboard", "frameworklab", "notes", "tools"])
        for row in tests:
            self.assertIn("--no-tests=error", row)
        module_configs = [row for row in commands if "-DWORKBENCH_TARGET_IOS=OFF" in row
                          and "-S" in row and "module-tests" in row[row.index("-B") + 1]]
        self.assertEqual(len(module_configs), 6)
        for row in module_configs:
            self.assertIn("-DWORKBENCH_TARGET_QT=OFF", row)
            self.assertIn("-DARIA_DEPENDENCIES_OFFLINE=ON", row)

    def test_cache_conflict_does_not_delete_existing_build(self):
        directory = self.root / "existing"
        directory.mkdir()
        cache = directory / "CMakeCache.txt"
        cache.write_text("CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
        command = ["cmake", "-S", str(self.root), "-B", str(directory), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"]
        with self.assertRaisesRegex(ValueError, "another --build-dir"):
            build.validate_cache(command)
        self.assertTrue(cache.exists())
        command[-1] = "-DCMAKE_BUILD_TYPE=Release"
        build.validate_cache(command)

    def test_matching_runtime_path_is_added_for_ctest(self):
        command = ["ctest", "--test-dir", str(self.root), "-C", "Debug"]
        env = build.command_environment(command, {"PATH": "original"})
        self.assertTrue(env["PATH"].startswith(str(self.root / "bin/Debug")))
        self.assertTrue(env["PATH"].endswith("original"))

    def test_invalid_jobs_rejected(self):
        for jobs in ("0", "-1", "257"):
            with self.subTest(jobs=jobs), self.assertRaises(SystemExit):
                build.arguments(["--jobs", jobs])


if __name__ == "__main__":
    unittest.main()
