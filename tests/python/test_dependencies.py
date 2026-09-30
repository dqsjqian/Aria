"""Dependency selection and lock safety regressions without network access."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "dependencies.py"
spec = importlib.util.spec_from_file_location("aria_dependencies", SCRIPT)
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)

SHA = "a" * 40
DIGEST = "b" * 64
SOURCE = {"provider": "github", "repo": "example/library", "artifact": "git", "tag_prefix": "v"}


class FakeContext:
    offline = False

    def __init__(self):
        self.calls = []
        self.releases = [
            {"tag_name": "v1.9.0"}, {"tag_name": "v2.0.0-rc1", "prerelease": True},
            {"tag_name": "v1.10.0"}, {"tag_name": "v99.0.0", "draft": True},
        ]
        self.tags = []
        self.annotation = None
        self.fail = False

    def github_json(self, path):
        self.calls.append(path)
        if self.offline or self.fail:
            raise AssertionError("Unexpected network lookup")
        if "/releases?" in path:
            return self.releases
        if "/tags?" in path:
            return self.tags
        if "/releases/tags/" in path:
            tag = path.rsplit("/", 1)[1]
            return next((x for x in self.releases if x["tag_name"] == tag), None)
        if "/git/ref/tags/" in path:
            return {"object": self.annotation or {"type": "commit", "sha": SHA}}
        if "/git/tags/" in path:
            return {"object": {"type": "commit", "sha": SHA}}
        raise AssertionError(path)

    def download_digest(self, url, expected_sha256=None):
        if self.offline:
            raise AssertionError("Unexpected download")
        self.calls.append(url)
        return DIGEST


class DependencyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aria-dependencies-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.manifest = self.root / "dependencies.json"
        self.lock = self.root / "dependencies.lock.json"
        self.context = FakeContext()
        self.write_manifest({"library": SOURCE})

    def write_manifest(self, dependencies):
        self.manifest.write_text(json.dumps({"schema": 1, "dependencies": dependencies}))

    def resolve(self, **kwargs):
        return deps.resolve(self.manifest, self.lock, context=self.context, **kwargs)

    def test_latest_means_highest_stable_release(self):
        record = self.resolve()["dependencies"]["library"]
        self.assertEqual(record["version"], "1.10.0")
        self.assertEqual(record["revision"], SHA)
        self.assertEqual(record["requested"], "latest")

    def test_locked_resolution_is_offline_and_does_not_rewrite(self):
        self.resolve()
        content = self.lock.read_bytes()
        timestamp = self.lock.stat().st_mtime_ns
        self.context.offline = True
        self.context.calls.clear()
        self.resolve()
        self.assertEqual(self.context.calls, [])
        self.assertEqual(self.lock.read_bytes(), content)
        self.assertEqual(self.lock.stat().st_mtime_ns, timestamp)

    def test_update_is_explicit_and_refreshes_latest(self):
        self.resolve()
        self.context.releases.append({"tag_name": "v2.0.0"})
        self.assertEqual(self.resolve()["dependencies"]["library"]["version"], "1.10.0")
        self.assertEqual(self.resolve(update=True)["dependencies"]["library"]["version"], "2.0.0")

    def test_explicit_version_wins_and_remains_locked(self):
        self.write_manifest({"library": {**SOURCE, "version": "1.9.0"}})
        result = self.resolve(versions={"library": "1.10.0"})
        self.assertEqual(result["dependencies"]["library"]["version"], "1.10.0")
        # Removing a command-line override restores an explicit manifest request.
        self.assertEqual(self.resolve()["dependencies"]["library"]["version"], "1.9.0")
        self.write_manifest({"library": SOURCE})
        self.resolve(versions={"library": "1.9.0"})
        self.context.offline = True
        self.assertEqual(self.resolve()["dependencies"]["library"]["version"], "1.9.0")

    def test_selecting_the_already_locked_version_needs_no_network(self):
        self.resolve()
        self.context.offline = True
        record = self.resolve(versions={"library": "1.10.0"})["dependencies"]["library"]
        self.assertEqual(record["requested"], "1.10.0")

    def test_unknown_version_override_is_rejected(self):
        with self.assertRaisesRegex(deps.DependencyError, "declared dependency"):
            self.resolve(versions={"typo": "1.0"})
        self.assertFalse(self.lock.exists())

    def test_explicit_branch_or_prerelease_is_rejected_before_network(self):
        for version in ["main", "2.0.0-rc1", "v2.0.0-beta", "../bad"]:
            with self.subTest(version=version), self.assertRaises(deps.DependencyError):
                self.resolve(versions={"library": version})
        self.assertEqual(self.context.calls, [])

    def test_override_outside_selection_is_not_silently_ignored(self):
        self.write_manifest({"library": SOURCE, "other": SOURCE})
        with self.assertRaisesRegex(deps.DependencyError, "selected by --only"):
            self.resolve(only=["library"], versions={"other": "1.9.0"})
        self.assertFalse(self.lock.exists())

    def test_malformed_locked_record_is_reported_without_mutation(self):
        self.lock.write_text(json.dumps({"schema": 1, "dependencies": {"library": []}}))
        before = self.lock.read_bytes()
        with self.assertRaisesRegex(deps.DependencyError, "Invalid locked"):
            self.resolve()
        self.assertEqual(self.lock.read_bytes(), before)

    def test_new_selection_offline_does_not_mutate_lock(self):
        self.resolve()
        old = self.lock.read_bytes()
        self.context.offline = True
        with self.assertRaisesRegex(deps.DependencyError, "matching lock"):
            self.resolve(versions={"library": "1.9.0"})
        self.assertEqual(self.lock.read_bytes(), old)

    def test_resolution_failure_is_transactional(self):
        self.resolve()
        old = self.lock.read_bytes()
        self.write_manifest({"library": SOURCE, "broken": {"provider": "unsupported"}})
        with self.assertRaisesRegex(deps.DependencyError, "Unsupported"):
            self.resolve(update=True)
        self.assertEqual(self.lock.read_bytes(), old)

    def test_effective_lock_preserves_source_and_unselected_records(self):
        self.resolve()
        original = self.lock.read_bytes()
        effective = self.root / "build" / "effective.json"
        result = deps.resolve(self.manifest, effective, base_lock=self.lock,
                              versions={"library": "1.9.0"}, context=self.context)
        self.assertEqual(self.lock.read_bytes(), original)
        self.assertEqual(result["dependencies"]["library"]["version"], "1.9.0")

    def test_lock_hash_revision_and_url_are_validated(self):
        record = self.resolve()["dependencies"]["library"]
        for changed in [{"revision": "main"}, {"url": "file:///tmp/private"},
                        {"source": {**SOURCE, "artifact": "archive"}, "sha256": "bad"}]:
            with self.subTest(changed=changed), self.assertRaises(deps.DependencyError):
                deps.validate_record({**record, **changed})

    def test_tag_only_projects_and_annotated_tags(self):
        self.context.releases = []
        self.context.tags = [{"name": "main"}, {"name": "v2.2.0"}, {"name": "v3.0.0-beta"}]
        self.context.annotation = {"type": "tag", "sha": "c" * 40}
        record = self.resolve()["dependencies"]["library"]
        self.assertEqual(record["tag"], "v2.2.0")
        self.assertEqual(record["revision"], SHA)

    def test_tag_separator_and_asset_templates(self):
        source = {**SOURCE, "tag_prefix": "curl-", "tag_separator": "_",
                  "artifact": "release-asset", "asset": "curl-{version}.tar.xz"}
        self.write_manifest({"library": source})
        self.context.releases = [{"tag_name": "curl-8_22_0", "assets": [
            {"name": "curl-8.22.0.tar.xz", "browser_download_url": "https://example.invalid/curl.tar.xz",
             "digest": "sha256:" + DIGEST}]}]
        record = self.resolve(versions={"library": "8.22.0"})["dependencies"]["library"]
        self.assertEqual(record["version"], "8.22.0")
        self.assertEqual(record["tag"], "curl-8_22_0")
        self.assertEqual(record["sha256"], DIGEST)
        self.assertEqual(record["checksum_source"], "github-release-asset")


if __name__ == "__main__":
    unittest.main()
