#!/usr/bin/env python3
"""Aria documentation toolchain in one entry.

Subcommands:
  filter     Doxygen input filter: keep repository links usable in flattened HTML.
  html       prepare: repair known Doxygen navigation defects; check: verify pages.
  api-check  Fail when docs reference Aria API symbols that do not exist.
  doxygen    Install the pinned upstream Doxygen binary (macOS ARM64/Intel).

Exit codes follow the historical per-tool contracts: 0 pass, 1 check failure,
2 setup error.
"""
from __future__ import annotations

import argparse
from html import escape, unescape
from html.parser import HTMLParser
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit, urlunsplit

# ══ filter: Doxygen input filter ═════════════════════════════════════════

def filter_markdown(text, source, root, repository, revision):
    def resolve(url):
        parts = urlsplit(url)
        if parts.scheme or parts.netloc:
            return url
        target = (source.parent / unquote(parts.path)).resolve() if parts.path else source
        if not target.is_relative_to(root) or not target.exists():
            return url
        # Doxygen resolves plain Markdown paths to generated pages. GitHub
        # section fragments are not Doxygen ids, so keep those on GitHub.
        if target.suffix.lower() == ".md" and parts.path and not parts.fragment:
            return url
        path = quote(target.relative_to(root).as_posix(), safe="/")
        ref = quote(revision, safe="")
        if target.suffix.lower() in {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp"}:
            # HTML <img> elements are passed through verbatim by Doxygen.
            repo_path = urlsplit(repository).path.strip("/")
            base = f"https://raw.githubusercontent.com/{repo_path}/{ref}/{path}"
        else:
            kind = "tree" if target.is_dir() else "blob"
            base = f"{repository.rstrip('/')}/{kind}/{ref}/{path}"
        return urlunsplit((*urlsplit(base)[:3], parts.query, parts.fragment))

    def markdown_link(match):
        url = match[2]
        if url.startswith("<") and url.endswith(">"):
            return match[1] + "<" + resolve(url[1:-1]) + ">"
        return match[1] + resolve(url)

    # A linked image (the README badges) has a nested label. Handle its outer
    # link first; the ordinary-link expression then processes the image URL.
    text = re.sub(r"(\[!\[[^\]\n]*\]\([^\n)]*\)\]\()(<[^>\n]+>|[^\s)]+)", markdown_link, text)
    text = re.sub(r"(!?\[[^\]\n]*\]\()(<[^>\n]+>|[^\s)]+)", markdown_link, text)
    return re.sub(
        r"(\b(?:href|src)\s*=\s*[\"'])([^\"']+)([\"'])",
        lambda match: match[1] + resolve(match[2]) + match[3],
        text,
    )


def filter_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Keep repository links usable when Doxygen flattens Markdown into HTML.")
    parser.add_argument("--revision", default="main")
    parser.add_argument("--repository", default="https://github.com/dqsjqian/Aria")
    parser.add_argument("source", type=Path)
    args = parser.parse_args(argv)
    source = args.source.resolve()
    text = source.read_text(encoding="utf-8")
    if source.suffix.lower() == ".md":
        root = Path(__file__).resolve().parent.parent
        text = filter_markdown(text, source, root, args.repository, args.revision)
    sys.stdout.write(text)
    return 0

# ══ html: prepare and check generated pages ══════════════════════════════

# ── prepare: known Doxygen 1.18 navigation defects ───────────────────────────
INTERNAL_PAGES = {
    "interface_aria_table_data_source.html",
    "interface_aria_u_i_table_data_source.html",
}
APPLE_PROTOCOL = "class_n_s_text_field_delegate-p.html"
INDEX_PAGE = re.compile(r"(?:functions|globals)(?:_[A-Za-z0-9_~]+)?\.html$")
LINK = re.compile(r"(<a\b[^>]*>)(.*?)</a>", re.DOTALL)
HREF = re.compile(r"\bhref=([\"'])(.*?)\1")


class Anchors(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.ids = set()
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if key in ("id", "name"):
                self.ids.add(value)


def prepare(root):
    pages = {p.resolve(): p.read_text(encoding="utf-8") for p in root.rglob("*.html")}
    anchors = {path: Anchors(text).ids for path, text in pages.items()}
    repaired = 0
    for source, text in pages.items():
        def rewrite(match):
            nonlocal repaired
            opening, label = match[1], match[2]
            href = HREF.search(opening)
            if not href:
                return match[0]
            url = urlsplit(unescape(href[2]))
            if url.scheme or url.netloc:
                return match[0]
            target = (source.parent / unquote(url.path)).resolve() if url.path else source
            if target.name in INTERNAL_PAGES and not target.exists():
                # EXCLUDE_SYMBOLS hides these private ObjC bridges, but 1.18
                # still emits links to them in verbatim header listings.
                repaired += 1
                return label
            if target.name == APPLE_PROTOCOL and not target.exists():
                destination = "https://developer.apple.com/documentation/appkit/nstextfielddelegate"
            elif (target in anchors and INDEX_PAGE.fullmatch(target.name)
                  and url.fragment.startswith("index_") and url.fragment not in anchors[target]):
                # The destructor letter is escaped in the target id but not
                # its links. Empty alphabetical sections have no target id:
                # link to their existing index page instead.
                fragment = "index__7E" if url.fragment == "index_~" and "index__7E" in anchors[target] else ""
                destination = urlunsplit((url.scheme, url.netloc, url.path, url.query, fragment)) or "#"
            else:
                return match[0]
            repaired += 1
            return (opening[:href.start(2)] + escape(destination, quote=True)
                    + opening[href.end(2):] + label + "</a>")

        updated = LINK.sub(rewrite, text)
        if updated != text:
            source.write_text(updated, encoding="utf-8")
    return repaired


# ── check: publication gate ──────────────────────────────────────────────────
# Representative public types across core, ABI, runtime, async, binding and
# every shipped adapter. This is a coverage smoke test, not a C++ API parser.
PUBLIC_PAGES = (
    "classaria_1_1reactive_1_1_property.html",
    "classaria_1_1_observable_list.html",
    "classaria_1_1_validator.html",
    "classaria_1_1_i_property.html",
    "classaria_1_1abi_1_1_signal_erased.html",
    "classaria_1_1runtime_1_1_container.html",
    "classaria_1_1async_1_1_task.html",
    "classaria_1_1async_1_1_channel.html",
    "classaria_1_1async_1_1_cancellation_source.html",
    "classaria_1_1binding_1_1_binding_engine.html",
    "classaria_1_1adapters_1_1qt6_1_1_qt_adapter.html",
    "classaria_1_1adapters_1_1qt6_1_1_observable_list_model.html",
    "classaria_1_1adapters_1_1appkit_1_1_app_kit_adapter.html",
    "classaria_1_1adapters_1_1appkit_1_1_observable_table_source.html",
    "classaria_1_1adapters_1_1uikit_1_1_u_i_kit_adapter.html",
    "classaria_1_1adapters_1_1uikit_1_1_observable_table_source.html",
    "classaria_1_1adapters_1_1jni_1_1_jni_adapter.html",
    "classaria_1_1adapters_1_1jni_1_1_jni_list_source.html",
    "classaria_1_1adapters_1_1http_1_1_http_adapter.html",
)

PUBLIC_FUNCTIONS = {
    "namespacearia_1_1adapters_1_1qt6.html": ("bind_combo_box_selection",),
    "classaria_1_1binding_1_1_binding_engine.html": ("bind_int_converted",),
}


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.links = []
        self.anchors = set()
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        for key in ("id", "name"):
            if key in attrs:
                self.anchors.add(attrs[key])
        for key in ("href", "src"):
            if key in attrs:
                self.links.append(attrs[key])


def check(root):
    errors = []
    for name in ("index.html", "annotated.html", "search/search.js", *PUBLIC_PAGES):
        if not (root / name).is_file():
            errors.append(f"missing required API output: {name}")
    html = {p.resolve(): p.read_text(encoding="utf-8") for p in root.rglob("*.html")}
    for name, functions in PUBLIC_FUNCTIONS.items():
        for function in functions:
            if not re.search(r">\s*" + re.escape(function) + r"\s*</a>", html.get(root / name, "")):
                errors.append(f"missing public API function: {name}: {function}")
    pages = {path: Page(text) for path, text in html.items()}
    for source, page in pages.items():
        for link in page.links:
            url = urlsplit(link)
            if url.scheme or url.netloc:
                continue
            target = (source.parent / unquote(url.path)).resolve() if url.path else source
            if target.is_dir():
                target /= "index.html"
            if not target.is_relative_to(root) or not target.is_file():
                errors.append(f"{source.relative_to(root)}: missing local target {link}")
            elif url.fragment and target in pages and unquote(url.fragment) not in pages[target].anchors:
                errors.append(f"{source.relative_to(root)}: missing anchor {link}")
    return len(pages), sorted(set(errors))


def html_main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Prepare generated Doxygen HTML for publication and strictly check it.")
    subparsers = parser.add_subparsers(dest="html_command", required=True)
    prepare_parser = subparsers.add_parser("prepare", help="repair known Doxygen navigation defects")
    prepare_parser.add_argument("html_directory", type=Path)
    check_parser = subparsers.add_parser("check", help="verify pages, local links, and assets")
    check_parser.add_argument("html_directory", type=Path)
    args = parser.parse_args(argv)
    if args.html_command == "prepare":
        print(f"docs-html prepare: repaired {prepare(args.html_directory.resolve())} Doxygen navigation links")
        return 0
    count, errors = check(args.html_directory.resolve())
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"docs-html check: {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"docs-html check: {count} HTML pages, {len(PUBLIC_PAGES)} public API samples, local links and assets OK")
    return 0


# ── api-check: fail when docs reference Aria API symbols that do not exist ───
API_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
API_REFERENCE = re.compile(r"aria::[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)+")
API_TAIL_TRIM = re.compile(r"[^A-Za-z0-9_:]+$")


def collect_nested_owners(sources: str) -> dict:
    """Indentation-aware pass: attribute indented type declarations to the
    most recent column-0 class/struct owner (mirrors the historical awk)."""
    nested = {}
    owner = ""
    for line in sources.splitlines():
        if re.match(r"^(class|struct)\s", line):
            name = re.sub(r"^(class|struct)\s+", "", line)
            name = re.sub(r"\s*(ARIA_[A-Z_]+\s+)", "", name, count=1)
            name = re.sub(r"\s*[:{;].*$", "", name)
            name = re.sub(r"^ARIA_[A-Z_]+\s+", "", name)
            name = name.rstrip()
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                owner = name
            continue
        if re.match(r"^};?", line):
            owner = ""
            continue
        if owner and re.match(r"^\s+(class|struct|enum(\s+class)?|using)\s", line):
            name = re.sub(r"^\s+", "", line)
            name = re.sub(r"^(class|struct|enum(\s+class)?|using)\s+", "", name)
            name = re.sub(r"\s*[:={;].*$", "", name)
            name = name.rstrip()
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) and name not in nested:
                nested[name] = owner
    return nested


def api_check_main(argv=None) -> int:
    repo_root = Path(__file__).resolve().parents[1]
    docs_dir = Path(os.environ.get("ARIA_DOCS_DIR") or repo_root / "docs")
    headers_dir = Path(os.environ.get("ARIA_HEADERS_DIR") or repo_root / "modules")
    if not docs_dir.is_dir():
        print(f"check-docs-api: docs dir not found: {docs_dir}", file=sys.stderr)
        return 2
    if not headers_dir.is_dir():
        print(f"check-docs-api: headers dir not found: {headers_dir}", file=sys.stderr)
        return 2
    source_texts = []
    for path in sorted(headers_dir.rglob("*")):
        if (path.suffix in (".hpp", ".h", ".cpp", ".mm")
                and "tests" not in path.parts and "fuzz" not in path.parts):
            source_texts.append(path.read_text(encoding="utf-8", errors="replace"))
    sources = "\n".join(source_texts)
    symbols = set(API_IDENTIFIER.findall(sources))
    namespaces = set()
    for match in re.finditer(r"namespace\s+([A-Za-z_][A-Za-z0-9_:]*)", sources):
        for part in match.group(1).split("::"):
            if part:
                namespaces.add(part)
    nested = collect_nested_owners(sources)
    missing = 0
    print(f"check-docs-api: scanning {docs_dir} against {headers_dir}")
    print()
    for doc in sorted(docs_dir.rglob("*.md")):
        doc_missing = []
        text = doc.read_text(encoding="utf-8", errors="replace")
        refs = sorted({API_TAIL_TRIM.sub("", match.group(0))
                       for match in API_REFERENCE.finditer(text)})
        for ref in refs:
            tail_sym = ref.rsplit("::", 1)[-1]
            if not tail_sym:
                continue
            if not tail_sym[0].isupper() and "_" not in tail_sym:
                continue
            if tail_sym not in symbols:
                doc_missing.append((ref, f"no such symbol '{tail_sym}'"))
                continue
            chain = ref[len("aria::"):]
            prev, prefix = "aria", "aria"
            for comp in chain.split("::"):
                if not comp:
                    continue
                if prev in namespaces:
                    owner = nested.get(comp)
                    if owner and owner != comp and owner not in namespaces:
                        pattern = re.compile(r"^(class|struct|enum(\s+class)?|using)\s+"
                                             r"(ARIA_[A-Z_]+\s+)?" + re.escape(comp) + r"\b",
                                             re.MULTILINE)
                        if not pattern.search(sources):
                            doc_missing.append(
                                (ref, f"'{comp}' is nested in '{owner}' — write {prefix}::{owner}::{comp}"))
                            break
                prefix = prefix + "::" + comp
                prev = comp
        if doc_missing:
            try:
                shown = doc.relative_to(repo_root)
            except ValueError:
                shown = doc
            print(f"✗ {shown}")
            for ref, message in doc_missing:
                print(f"  {ref:<52} {message}")
            print()
            missing += len(doc_missing)
    if missing:
        print(f"check-docs-api: FAILED — {missing} documented reference(s) do not resolve.", file=sys.stderr)
        print(file=sys.stderr)
        print("Each line above is a fully-qualified aria:: name that the docs tell users to", file=sys.stderr)
        print("write but the headers do not support at that scope. Either fix the doc to", file=sys.stderr)
        print("match the real API, or add the API.", file=sys.stderr)
        return 1
    print("check-docs-api: OK — every fully-qualified aria:: reference in the docs resolves.")
    if docs_dir == repo_root / "docs":
        doc_text = (repo_root / "docs/guide/adapters/http.md").read_text(encoding="utf-8")
        block = doc_text.split("<!-- BEGIN COMPILED HTTP EXAMPLE -->", 1)[1].split(
            "<!-- END COMPILED HTTP EXAMPLE -->", 1)[0]
        code = block.split("```cpp\n", 1)[1].rsplit("```", 1)[0]
        expected = (repo_root / "modules/adapters/http/tests/http_guide_example.cpp").read_text(encoding="utf-8")
        if code.strip() != expected.strip():
            print("HTTP guide example differs from the compiled CTest source", file=sys.stderr)
            return 1
        print("check-docs-api: compiled HTTP example is in sync")
    return 0


# ── doxygen: install the pinned documentation generator ─────────────────────
DOXYGEN_ASSETS = {
    ("Darwin", "arm64"): ("doxygen-1.18.0-mac-arm.zip",
                          "31d1c74467a9f6f456b4e6a4eff0c03e93d1ffa92da5fdf2d53a2f14ebaabbd7"),
    ("Darwin", "x86_64"): ("doxygen-1.18.0-mac-intel.zip",
                           "8045d72f6f900fd430042339c1493d6eefcc85a7f990361fb57685682f16a2f9"),
}


def doxygen_main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Install the verified upstream Doxygen 1.18.0 binary without changing Homebrew/PATH (Homebrew 1.18.0 can intermittently SIGBUS on ARM64).")
    parser.add_argument("destination", type=Path)
    args = parser.parse_args(argv)
    key = (platform.system(), platform.machine())
    if key not in DOXYGEN_ASSETS:
        print("docs.py doxygen: this installer supports macOS ARM64 and Intel", file=sys.stderr)
        return 1
    asset, checksum = DOXYGEN_ASSETS[key]
    with tempfile.TemporaryDirectory() as staging:
        staging = Path(staging)
        archive = staging / asset
        url = f"https://github.com/doxygen/doxygen/releases/download/Release_1_18_0/{asset}"
        with urllib.request.urlopen(url) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != checksum:
            print("docs.py doxygen: upstream Doxygen archive checksum mismatch", file=sys.stderr)
            return 1
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(staging / "extracted")
        destination = args.destination
        (destination / "bin").mkdir(parents=True, exist_ok=True)
        executable = destination / "bin" / "doxygen"
        shutil.copyfile(staging / "extracted" / "doxygen-1.18.0" / "doxygen", executable)
        executable.chmod(0o755)
        print(subprocess.run([str(executable), "--version"], capture_output=True,
                             text=True).stdout.strip())
    return 0



def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="docs.py",
        description="Aria documentation toolchain: Doxygen input filter, HTML preparation and checks, docs API reference validation and the pinned Doxygen installer.")
    sub = parser.add_subparsers(dest="command", required=True)
    filter_parser = sub.add_parser("filter", help="Doxygen input filter for repository links")
    filter_parser.add_argument("--revision", default="main")
    filter_parser.add_argument("--repository", default="https://github.com/dqsjqian/Aria")
    filter_parser.add_argument("source", type=Path)
    html_parser = sub.add_parser("html", help="prepare generated Doxygen HTML and strictly check it")
    html_sub = html_parser.add_subparsers(dest="html_command", required=True)
    html_prepare = html_sub.add_parser("prepare", help="repair known Doxygen navigation defects")
    html_prepare.add_argument("html_directory", type=Path)
    html_check = html_sub.add_parser("check", help="verify pages, local links, and assets")
    html_check.add_argument("html_directory", type=Path)
    sub.add_parser("api-check", help="fail if docs reference Aria API symbols that do not exist in the headers")
    doxygen_parser = sub.add_parser("doxygen", help="install the pinned Doxygen generator")
    doxygen_parser.add_argument("destination", type=Path)
    args = parser.parse_args(argv)
    if args.command == "filter":
        return filter_main(["--revision", args.revision, "--repository", args.repository, str(args.source)])
    if args.command == "html":
        return html_main([args.html_command, str(args.html_directory)])
    if args.command == "api-check":
        return api_check_main([])
    if args.command == "doxygen":
        return doxygen_main([str(args.destination)])
    return 2


if __name__ == "__main__":
    sys.exit(main())
