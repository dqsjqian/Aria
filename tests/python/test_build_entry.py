"""Offline contracts for the portable build entry; no toolchains or network needed."""
import importlib.util
import json
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

    def test_extra_arguments_cannot_redirect_configure_or_override_configuration(self):
        self.layout("tutorial")
        for argument in ("-Belsewhere", "-Selsewhere", "--fresh", "--preset=other",
                         "-DCMAKE_GENERATOR:STRING=Ninja", "-DCMAKE_BUILD_TYPE:STRING=Debug"):
            with self.subTest(argument=argument), self.assertRaisesRegex(ValueError, "cmake-arg"):
                self.plan("--cmake-arg=" + argument)
        self.plan("--cmake-arg=-DCMAKE_BUILD_TYPE:STRING=Release")

    def test_selected_source_and_shell_cannot_be_overridden_by_extra_definition(self):
        self.layout("tools")
        for argument in ("-DARIA_DIR:PATH=/other/aria", "-DWORKBENCH_TARGET_QT:BOOL=ON"):
            with self.subTest(argument=argument), self.assertRaisesRegex(ValueError, "conflicts"):
                self.plan("--platform", "web", "--cmake-arg=" + argument)

    def test_apple_architecture_reaches_application_and_all_module_projects(self):
        self.layout("tools")
        commands = self.plan("--platform", "web", "--arch", "x86_64", "--test", host="Darwin")
        configs = [row for row in commands if row[0] == "cmake" and "-S" in row]
        self.assertEqual(len(configs), 7)
        for command in configs:
            self.assertIn("-DCMAKE_OSX_ARCHITECTURES=x86_64", command)

    def test_architecture_is_not_just_a_build_directory_label(self):
        self.layout("aria")
        with self.assertRaisesRegex(ValueError, "--arch"):
            self.plan("--arch", "arm64", host="Linux")
        with self.assertRaisesRegex(ValueError, "Android ABI"):
            self.plan("--platform", "android", "--ndk", "ndk", "--arch", "arm64")

    def test_msvc_ninja_selects_msvc_and_mingw_rejects_visual_studio(self):
        self.layout("aria")
        command = self.plan("--toolchain", "msvc", "--generator", "Ninja", host="Windows")[0]
        self.assertIn("-DCMAKE_CXX_COMPILER=cl", command)
        with self.assertRaisesRegex(ValueError, "MinGW-compatible"):
            self.plan("--toolchain", "mingw", "--generator", "Visual Studio 17 2022", host="Windows")

    def test_visual_studio_platform_reaches_all_projects_and_guards_cache(self):
        self.layout("tools")
        commands = self.plan("--generator", "Visual Studio 18 2026", "--generator-platform", "ARM64", "--test", host="Windows")
        configs = [row for row in commands if row[0] == "cmake" and "-S" in row]
        self.assertEqual(len(configs), 7)
        for command in configs:
            self.assertEqual(command[command.index("-A") + 1], "ARM64")
        directory = Path(configs[0][configs[0].index("-B") + 1])
        directory.mkdir(parents=True)
        (directory / "CMakeCache.txt").write_text("CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "CMAKE_GENERATOR_PLATFORM"):
            build.validate_cache(configs[0], env={})
        with self.assertRaisesRegex(ValueError, "Visual Studio"):
            self.plan("--generator", "Ninja", "--generator-platform", "x64", host="Windows")

    def test_tutorial_sdk_uses_locked_exact_version_without_fetch_and_keeps_qt_prefix(self):
        self.layout("tutorial")
        lock = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))
        # Aria itself is not its own dependency; its source consumer fixture uses
        # the same known complete Git lock record as the downstream projects.
        if "aria" not in lock["dependencies"]:
            lock["dependencies"]["aria"] = {"artifact": "git", "provider": "github", "repo": "dqsjqian/Aria", "tag_prefix": "v", "resolved": {
                "version": "3.1.1", "requested": "latest", "revision": "a56ad396433f5278fba81d920ddd86d1fe5aa923",
                "url": "https://github.com/dqsjqian/Aria.git", "request_hash": "2c6620ad60a1854f8c761cdfd7caebc063116d49a2d34f36c920a1a617d014ce"}}
        version = lock["dependencies"]["aria"]["resolved"]["version"]
        (self.root / "dependencies.json").write_text(json.dumps(lock), encoding="utf-8")
        sdk, qt = self.root / "SDK with spaces", self.root / "Qt"
        command = self.plan("--aria-prefix", str(sdk), "--qt-prefix", str(qt), "--platform", "qt")[0]
        self.assertEqual(command[0], "cmake")
        self.assertIn("-DARIA_ROOT=", command)
        self.assertIn(f"-DARIA_SDK_PREFIX={sdk.resolve()}", command)
        self.assertIn(f"-DARIA_DEP_ARIA_VERSION={version}", command)
        self.assertIn(f"-DCMAKE_PREFIX_PATH={sdk.resolve()};{qt.resolve()}", command)
        self.assertTrue(command[command.index("-B") + 1].endswith("-sdk"))
        self.assertEqual(self.plan("deps", "--aria-prefix", str(sdk)), [])
        with self.assertRaisesRegex(ValueError, "conflicts"):
            self.plan("--aria-prefix", str(sdk), "--cmake-arg=-DARIA_DEP_ARIA_VERSION=3.0.0")
        lock["dependencies"]["aria"]["version"] = "999.0.0"
        (self.root / "dependencies.json").write_text(json.dumps(lock), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "declaration changed"):
            self.plan("--aria-prefix", str(sdk))

    def test_sdk_requires_a_lock_and_is_tutorial_only(self):
        self.layout("tutorial")
        (self.root / "dependencies.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "resolved Aria version"):
            self.plan("--aria-prefix", "sdk")
        with self.assertRaises(SystemExit):
            build.arguments(["--aria-prefix", "sdk", "--aria-root", "source"])
        (self.root / "core/agent").mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "only to AriaTutorial"):
            self.plan("--aria-prefix", "sdk")

    def test_cache_checks_typed_definitions_and_source_sdk_mode_changes(self):
        directory = self.root / "cache"
        directory.mkdir()
        cache = directory / "CMakeCache.txt"
        command = ["cmake", "-S", str(self.root), "-B", str(directory)]
        conflicts = (("CMAKE_TOOLCHAIN_FILE", "/old/toolchain", "/new/toolchain"),
                     ("CMAKE_CXX_COMPILER", "/old/compiler", "/new/compiler"),
                     ("CMAKE_OSX_ARCHITECTURES", "arm64", "x86_64"),
                     ("ARIA_DIR", "/old/source", "/new/source"),
                     ("ARIA_ROOT", "/source", ""),
                     ("ARIA_ROOT", "", "/source"),
                     ("ARIA_SDK_PREFIX", "/sdk/old", "/sdk/new"),
                     ("ARIA_DEP_ARIA_VERSION", "3.1.0", "3.1.1"),
                     ("CMAKE_PREFIX_PATH", "/sdk;/qt/old", "/sdk;/qt/new"))
        for key, old, new in conflicts:
            with self.subTest(key=key, old=old):
                content = f"{key}:STRING={old}\n"
                cache.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, key):
                    build.validate_cache(command + [f"-D{key}:STRING={new}"], env={})
                self.assertEqual(cache.read_text(encoding="utf-8"), content)

    def test_prefix_cache_accepts_equivalent_paths_but_preserves_search_order(self):
        directory = self.root / "cache"
        directory.mkdir()
        first, second = self.root / "Qt", self.root / "SDK"
        first.mkdir()
        second.mkdir()
        (directory / "CMakeCache.txt").write_text(
            f"CMAKE_PREFIX_PATH:STRING={first};{second}\n", encoding="utf-8")
        command = ["cmake", "-S", str(self.root), "-B", str(directory)]
        build.validate_cache(command + [f"-DCMAKE_PREFIX_PATH={first}/../Qt;{second}"], env={})
        with self.assertRaisesRegex(ValueError, "CMAKE_PREFIX_PATH"):
            build.validate_cache(command + [f"-DCMAKE_PREFIX_PATH={second};{first}"], env={})
        with self.assertRaisesRegex(ValueError, "CMAKE_PREFIX_PATH"):
            build.validate_cache(command + [f"-DCMAKE_PREFIX_PATH={first};;{second}"], env={})
        link = self.root / "Qt-link"
        try:
            link.symlink_to(first, target_is_directory=True)
        except OSError:
            return  # The canonical-path and order checks above apply on every host.
        build.validate_cache(command + [f"-DCMAKE_PREFIX_PATH={link};{second}"], env={})

    def test_cached_toolchain_does_not_leak_into_unrequested_native_build(self):
        directory = self.root / "cache"
        directory.mkdir()
        (directory / "CMakeCache.txt").write_text("CMAKE_TOOLCHAIN_FILE:FILEPATH=/android/toolchain\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "CMAKE_TOOLCHAIN_FILE"):
            build.validate_cache(["cmake", "-S", str(self.root), "-B", str(directory)], env={})

    def test_environment_compiler_and_generator_changes_are_checked(self):
        directory = self.root / "cache"
        directory.mkdir()
        (directory / "CMakeCache.txt").write_text("CMAKE_CXX_COMPILER:FILEPATH=/old/cxx\nCMAKE_GENERATOR:INTERNAL=Ninja\n", encoding="utf-8")
        command = ["cmake", "-S", str(self.root), "-B", str(directory)]
        for env in ({"CXX": "/new/cxx"}, {"CMAKE_GENERATOR": "Unix Makefiles"}):
            with self.subTest(env=env), self.assertRaisesRegex(ValueError, "conflicts"):
                build.validate_cache(command, env)

    def test_multi_config_cache_does_not_treat_empty_build_type_as_conflict(self):
        directory = self.root / "cache"
        directory.mkdir()
        (directory / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=\nCMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n", encoding="utf-8")
        build.validate_cache(["cmake", "-S", str(self.root), "-B", str(directory), "-DCMAKE_BUILD_TYPE=Release"], env={})

    def test_all_caches_are_checked_before_dependency_fetch(self):
        fetch = [sys.executable, "fetch_aria.py"]
        configure = ["cmake", "-S", str(self.root), "-B", str(self.root / "build")]
        with mock.patch.object(build, "plan", return_value=[fetch, configure]), \
             mock.patch.object(build, "validate_cache", side_effect=[None, ValueError("conflicting cache")]), \
             mock.patch.object(build.subprocess, "run") as run:
            self.assertEqual(build.main([]), 2)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
