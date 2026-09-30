#!/usr/bin/env python3
"""Resolve stable releases in one dependency file with embedded locked results.

Normal resolution reuses matching locked records without network access.
Only missing/changed requests, or an explicit update, query upstream releases.
This module uses the Python standard library; gh is optional for GitHub access.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request


class DependencyError(ValueError):
    """A dependency request cannot be resolved or verified."""


def load_json(path: Path, *, optional: bool = False) -> dict:
    if optional and not path.exists():
        return {"schema": 2, "dependencies": {}}
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict) or value.get("schema") != 2:
        raise DependencyError(f"Unsupported dependency schema: {path}")
    if not isinstance(value.get("dependencies"), dict):
        raise DependencyError(f"Missing dependency dictionary: {path}")
    return value


def dependency_spec(entry: dict) -> dict:
    if not isinstance(entry, dict):
        raise DependencyError("Dependency entries must be objects")
    return {key: value for key, value in entry.items() if key != "resolved"}


def request_hash(spec: dict) -> str:
    """Portable length-prefixed UTF-8 encoding, also implemented by CMake."""
    encoded = bytearray(b"aria-dependency-request-v1\n")
    for key, value in sorted(spec.items()):
        if not re.fullmatch(r"[a-z][a-z0-9_]*", key) or not isinstance(value, str):
            raise DependencyError("Dependency declarations require simple string fields")
        for part in (key, value):
            data = part.encode("utf-8")
            encoded.extend(str(len(data)).encode("ascii") + b":" + data)
    return hashlib.sha256(encoded).hexdigest()


def _entry_record(entry: dict, *, allow_missing: bool = False) -> dict | None:
    spec = dependency_spec(entry)
    fingerprint = request_hash(spec)
    resolved = entry.get("resolved")
    if resolved is None and allow_missing:
        return None
    if not isinstance(resolved, dict):
        raise DependencyError("Missing or invalid resolved dependency result")
    if resolved.get("request_hash") != fingerprint:
        if allow_missing:
            return None
        raise DependencyError("Dependency declaration changed; resolve it before building")
    record = {**resolved, "source": spec}
    validate_record(record)
    return record


def read_resolved(file: Path, only: list | None = None) -> dict:
    """Read/verify selected results without discovery or changing the file.

    The returned source field is an in-memory compatibility view, not another
    persisted declaration. A prior CLI override is accepted here; resolve()
    alone performs version selection when a new invocation requests it.
    """
    entries = load_json(file)["dependencies"]
    selected = set(only or entries)
    if selected - entries.keys():
        raise DependencyError("Selections must name a declared dependency")
    return {"schema": 1, "dependencies": {
        name: _entry_record(entries[name]) for name in sorted(selected)
    }}


def atomic_json(path: Path, value: dict) -> None:
    content = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


@contextlib.contextmanager
def lock_output(path: Path):
    # Keep coordination files outside source roots, including when the lock
    # file itself is a version-controlled build input.
    directory = Path(tempfile.gettempdir()) / "aria-dependency-locks"
    directory.mkdir(exist_ok=True)
    key = hashlib.sha256(str(path.resolve()).encode()).hexdigest()
    with (directory / key).open("a+b") as stream:
        if os.name == "nt":
            import msvcrt
            stream.seek(0, os.SEEK_END)
            if stream.tell() == 0:
                stream.write(b"\0")
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(stream, fcntl.LOCK_UN)


def https_url(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" or not parts.hostname or parts.username or parts.password:
        raise DependencyError("Dependency downloads require an HTTPS URL without credentials")
    return url


class Context:
    def __init__(self, cache_dir: Path, offline: bool = False):
        self.cache_dir = cache_dir
        self.offline = offline

    def get_bytes(self, url: str) -> bytes:
        if self.offline:
            raise DependencyError("Offline resolution requires a matching lock record")
        request = urllib.request.Request(https_url(url), headers={"User-Agent": "aria-dependencies"})
        with urllib.request.urlopen(request, timeout=120) as response:
            https_url(response.url)
            return response.read()

    def get_text(self, url: str) -> str:
        return self.get_bytes(url).decode("utf-8")

    def github_json(self, path: str):
        if self.offline:
            raise DependencyError("Offline resolution requires a matching lock record")
        if not path.startswith("repos/") or ".." in path.split("/"):
            raise DependencyError("Invalid GitHub API path")
        # gh uses its existing credential store without exposing a token to
        # this process or to release-asset download hosts.
        if shutil.which("gh"):
            result = subprocess.run(["gh", "api", "--hostname", "github.com", path],
                                    capture_output=True, text=True, check=False)
            if result.returncode == 0:
                return json.loads(result.stdout)
            if "404" in result.stderr:
                return None
        try:
            return json.loads(self.get_text("https://api.github.com/" + path))
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            raise DependencyError(f"GitHub API request failed with HTTP {error.code}") from error

    def download_digest(self, url: str, expected_sha256: str | None = None) -> str:
        if self.offline:
            raise DependencyError("Offline resolution cannot discover a new archive checksum")
        https_url(url)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        # Discovery always downloads afresh when no official digest exists.
        # A mutable upstream URL must not silently reuse an obsolete archive
        # during an explicit update. Normal locked builds never call here.
        with tempfile.TemporaryDirectory(prefix="resolve-", dir=self.cache_dir) as temporary:
            archive = Path(temporary) / "download"
            request = urllib.request.Request(url, headers={"User-Agent": "aria-dependencies"})
            with urllib.request.urlopen(request, timeout=120) as response, archive.open("wb") as output:
                https_url(response.url)
                while chunk := response.read(1024 * 1024):
                    digest.update(chunk)
                    output.write(chunk)
            actual = digest.hexdigest()
            if expected_sha256 and actual != expected_sha256.lower():
                raise DependencyError("Downloaded dependency does not match its published SHA256")
            destination = self.cache_dir / actual
            if destination.is_symlink():
                raise DependencyError("Resolver cache archive must not be a symbolic link")
            if destination.exists():
                if not destination.is_file() or hashlib.sha256(destination.read_bytes()).hexdigest() != actual:
                    raise DependencyError("Resolver cache archive was modified")
            else:
                os.replace(archive, destination)
            return actual


def tag_version(tag: str, spec: dict) -> str | None:
    prefix = spec.get("tag_prefix", "v")
    if not tag.startswith(prefix):
        return None
    version = tag[len(prefix):].replace(spec.get("tag_separator", "."), ".")
    # Never turn a prerelease, nightly, branch name or floating tag into the
    # default stable release. Explicit versions still need a real Git tag.
    if re.fullmatch(r"\d+(?:\.\d+){0,3}(?:\+[0-9A-Za-z.-]+)?", version):
        return version
    return None


def version_key(version: str) -> tuple:
    return tuple(int(part) for part in version.split("+")[0].split("."))


def github_pages(context: Context, path: str):
    for page in range(1, 101):
        values = context.github_json(f"{path}?per_page=100&page={page}")
        if values is None:
            return
        if not isinstance(values, list):
            raise DependencyError("Unexpected GitHub release listing")
        yield from values
        if len(values) < 100:
            return
    raise DependencyError("GitHub release listing exceeded the supported pagination limit")


def tag_revision(context: Context, repo: str, tag: str) -> str:
    reference = context.github_json(f"repos/{repo}/git/ref/tags/{urllib.parse.quote(tag, safe='')}")
    if not reference or "object" not in reference:
        raise DependencyError(f"Release tag was not found: {repo} {tag}")
    obj = reference["object"]
    visited = set()
    while obj.get("type") == "tag":
        sha = obj.get("sha", "")
        if sha in visited or len(visited) >= 10:
            raise DependencyError("Invalid annotated tag chain")
        visited.add(sha)
        annotation = context.github_json(f"repos/{repo}/git/tags/{sha}")
        if not annotation:
            raise DependencyError("Annotated tag object was not found")
        obj = annotation["object"]
    if obj.get("type") != "commit" or not re.fullmatch(r"[0-9a-f]{40}", obj.get("sha", "")):
        raise DependencyError("Dependency tag must resolve to a full Git commit")
    return obj["sha"]


def resolve_github(spec: dict, requested: str, context: Context) -> dict:
    repo = spec.get("repo", "")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise DependencyError("GitHub dependencies require an owner/repository")
    artifact = spec.get("artifact", "archive")
    if artifact not in {"archive", "file", "release-asset", "git"}:
        raise DependencyError("Unknown GitHub artifact type")
    release = None
    if requested == "latest":
        candidates = []
        for candidate in github_pages(context, f"repos/{repo}/releases"):
            version = tag_version(candidate.get("tag_name", ""), spec)
            if version and not candidate.get("draft") and not candidate.get("prerelease"):
                candidates.append((version_key(version), version, candidate))
        if candidates:
            _, version, release = max(candidates, key=lambda item: item[0])
            tag = release["tag_name"]
        elif artifact != "release-asset":
            tags = [(tag_version(item.get("name", ""), spec), item.get("name", ""))
                    for item in github_pages(context, f"repos/{repo}/tags")]
            tags = [(version, tag) for version, tag in tags if version]
            if not tags:
                raise DependencyError(f"No stable release tag was found for {repo}")
            version, tag = max(tags, key=lambda item: version_key(item[0]))
        else:
            raise DependencyError(f"No stable release with published assets was found for {repo}")
    else:
        prefix = spec.get("tag_prefix", "v")
        version = requested[len(prefix):] if prefix and requested.startswith(prefix) else requested
        version = version.replace(spec.get("tag_separator", "."), ".")
        separator = spec.get("tag_separator", ".")
        tag = spec.get("tag_template", "{prefix}{separated_version}").format(
            prefix=prefix, version=version, version_underscore=version.replace(".", "_"),
            separated_version=version.replace(".", separator))
        if requested.strip() != requested or tag_version(tag, spec) != version:
            raise DependencyError("Explicit dependency versions must be stable release versions")
        release = context.github_json(f"repos/{repo}/releases/tags/{urllib.parse.quote(tag, safe='')}")
        if release and (release.get("draft") or release.get("prerelease")):
            raise DependencyError("Dependency versions must select stable, published releases")
    revision = tag_revision(context, repo, tag)
    record = {"version": version, "tag": tag, "revision": revision}
    if artifact == "git":
        record.update(url=f"https://github.com/{repo}.git", sha256="")
    elif artifact == "archive":
        record["url"] = f"https://codeload.github.com/{repo}/tar.gz/{revision}"
        record["sha256"] = context.download_digest(record["url"])
        record["checksum_source"] = "downloaded-over-https"
    elif artifact == "file":
        path = spec.get("path", "")
        if not path or path.startswith("/") or ".." in path.split("/"):
            raise DependencyError("Invalid repository file path")
        record["url"] = f"https://raw.githubusercontent.com/{repo}/{revision}/{path}"
        record["sha256"] = context.download_digest(record["url"])
        record["checksum_source"] = "downloaded-over-https"
    else:
        if not release:
            raise DependencyError(f"The requested release has no asset metadata: {repo} {tag}")
        asset_name = spec["asset"].format(version=version, tag=tag,
                                        version_underscore=version.replace(".", "_"))
        assets = [asset for asset in release.get("assets", []) if asset.get("name") == asset_name]
        if len(assets) != 1:
            raise DependencyError(f"Expected exactly one release asset named {asset_name}")
        asset = assets[0]
        record["url"] = https_url(asset["browser_download_url"])
        official = asset.get("digest") or ""
        if re.fullmatch(r"sha256:[0-9a-fA-F]{64}", official):
            record["sha256"] = official[7:].lower()
            record["checksum_source"] = "github-release-asset"
        else:
            record["sha256"] = context.download_digest(record["url"])
            record["checksum_source"] = "downloaded-over-https"
    return record


def validate_record(record: dict) -> None:
    if not isinstance(record, dict) or not isinstance(record.get("source"), dict):
        raise DependencyError("Invalid locked dependency record")
    if not isinstance(record.get("version"), str) or not record["version"]:
        raise DependencyError("Locked dependency has no resolved version")
    if not isinstance(record.get("requested"), str) or not record["requested"]:
        raise DependencyError("Locked dependency has no version request")
    https_url(record.get("url", ""))
    if record["source"].get("provider") == "github":
        if not re.fullmatch(r"[0-9a-f]{40}", record.get("revision", "")):
            raise DependencyError("Locked GitHub dependency must contain a full commit")
    if record["source"].get("artifact") != "git" and not re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", "")):
        raise DependencyError("Locked download must contain SHA256")


def resolve_record(spec: dict, requested: str, context: Context) -> dict:
    if spec.get("provider") == "github":
        record = resolve_github(spec, requested, context)
    elif spec.get("provider") in {"sqlite", "quickjs"}:
        from dependency_sources import resolve_quickjs, resolve_sqlite
        provider = resolve_sqlite if spec["provider"] == "sqlite" else resolve_quickjs
        record = provider(spec, requested, context)
    else:
        raise DependencyError(f"Unsupported dependency provider: {spec.get('provider')}")
    record.update(requested=requested, source=spec)
    validate_record(record)
    return record


def resolve(file: Path, effective_file: Path | None = None, *,
            versions: dict | None = None, only: list | None = None,
            update: bool = False, context: Context | None = None) -> dict:
    """Resolve selected entries atomically in one file, or an isolated output.

    An effective output binds the complete input-file hash, so editing the
    checked-in resolution cannot resurrect an older build-directory selection.
    Returns a verified in-memory view for the selected names only.
    """
    output = effective_file or file
    versions = versions or {}
    context = context or Context(file.parent / "build/deps/resolver")
    with lock_output(output):
        document = load_json(file)
        entries = document["dependencies"]
        selected = set(only or entries)
        if (set(versions) | selected) - entries.keys():
            raise DependencyError("Version overrides and selections must name a declared dependency")
        if set(versions) - selected:
            raise DependencyError("Every version override must also be selected by --only")
        separate = output.resolve() != file.resolve()
        source_hash = hashlib.sha256(file.read_bytes()).hexdigest()
        previous = load_json(output, optional=True) if separate else document
        effective_entries = (previous["dependencies"]
                             if separate and previous.get("_base_sha256") == source_hash else {})
        # Copy declaration/result objects so no partial mutation is exposed on
        # disk if a later selected dependency fails to resolve.
        result = {"schema": 2, "dependencies": json.loads(json.dumps(entries))}
        if separate:
            result["_base_sha256"] = source_hash
            for name, entry in effective_entries.items():
                if name in entries and dependency_spec(entry) == dependency_spec(entries[name]):
                    old = _entry_record(entry, allow_missing=True)
                    if old:
                        result["dependencies"][name]["resolved"] = dict(entry["resolved"])
        for name in sorted(selected):
            spec = dependency_spec(entries[name])
            fingerprint = request_hash(spec)
            requested = versions.get(name, spec.get("version") or "latest")
            if not isinstance(requested, str) or not requested.strip():
                raise DependencyError(f"Invalid version selector: {name}")
            explicit = name in versions or spec.get("version") not in (None, "", "latest")
            record = None
            if not update:
                candidates = [entries[name]]
                if name in effective_entries and dependency_spec(effective_entries[name]) == spec:
                    candidates.append(effective_entries[name])
                for candidate in candidates:
                    old = _entry_record(candidate, allow_missing=True)
                    if old and (not explicit or requested == old.get("requested")
                                or requested in {old.get("version"), old.get("tag")}):
                        record = dict(old)
                        if explicit:
                            record["requested"] = requested
                        break
            if record is None:
                if context.offline:
                    raise DependencyError(f"No matching resolved result for {name} ({requested}); resolve online first")
                record = resolve_record(spec, requested, context)
            result["dependencies"][name]["resolved"] = {
                **{key: value for key, value in record.items() if key != "source"},
                "request_hash": fingerprint,
            }
        atomic_json(output, result)
        return read_resolved(output, only=sorted(selected))


def main(argv=None, *, command=None, file=None) -> int:
    description = ("Update dependency selections atomically. Explicit --version overrides "
                   "manifest versions; unspecified versions resolve the latest stable release. "
                   "Run the normal build and tests after reviewing the dependency-file diff.") if command == "update" else __doc__
    parser = argparse.ArgumentParser(description=description)
    if command is None:
        parser.add_argument("command", choices=("resolve", "update"))
    else:
        parser.set_defaults(command=command)
    parser.add_argument("--file", type=Path, default=file, required=file is None,
                        help="Single dependency file containing requirements and resolved results")
    parser.add_argument("--output", type=Path,
                        help="Optional isolated effective file; normally update --file in place")
    parser.add_argument("--cache-dir", type=Path, help="Download checksum discovery cache")
    parser.add_argument("--version", action="append", default=[], metavar="NAME=VERSION",
                        help="Override one version for this invocation; repeat for different names")
    parser.add_argument("--only", action="append", metavar="NAME",
                        help="Process only this name; repeat to select multiple dependencies")
    parser.add_argument("--offline", action="store_true",
                        help="Require matching locked metadata without any upstream lookup")
    args = parser.parse_args(argv)
    versions = {}
    try:
        for override in args.version:
            name, separator, version = override.partition("=")
            if not separator or not name or not version or name in versions:
                raise DependencyError("Use each --version NAME=VERSION override at most once")
            versions[name] = version
        result = resolve(args.file, args.output,
                         versions=versions, only=args.only, update=args.command == "update",
                         context=Context(args.cache_dir or args.file.parent / "build/deps/resolver",
                                         offline=args.offline))
        for name in sorted(args.only or result["dependencies"]):
            record = result["dependencies"][name]
            print(f"{name}: {record['version']} (requested {record['requested']}; locked)")
        return 0
    except (ValueError, OSError, KeyError) as error:
        print(f"dependency resolution failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
