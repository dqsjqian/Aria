#!/usr/bin/env python3
"""Prepare generated Doxygen HTML for publication and strictly check it.

prepare: repair known Doxygen navigation defects before link validation.
check: verify API pages, local links, and assets before publication.
"""

import argparse
from html import escape, unescape
from html.parser import HTMLParser
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit, urlunsplit


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
    "classaria_1_1observable_list.html",
    "classaria_1_1validator.html",
    "classaria_1_1i_property.html",
    "classaria_1_1abi_1_1signal_erased.html",
    "classaria_1_1runtime_1_1container.html",
    "classaria_1_1async_1_1_task.html",
    "classaria_1_1async_1_1_channel.html",
    "classaria_1_1async_1_1_cancellation_source.html",
    "classaria_1_1binding_1_1_binding_engine.html",
    "classaria_1_1adapters_1_1qt6_1_1_qt_adapter.html",
    "classaria_1_1adapters_1_1qt6_1_1observable_list_model.html",
    "classaria_1_1adapters_1_1appkit_1_1_app_kit_adapter.html",
    "classaria_1_1adapters_1_1appkit_1_1observable_table_source.html",
    "classaria_1_1adapters_1_1uikit_1_1_u_i_kit_adapter.html",
    "classaria_1_1adapters_1_1uikit_1_1observable_table_source.html",
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare", help="repair known Doxygen navigation defects")
    prepare_parser.add_argument("html_directory", type=Path)
    check_parser = subparsers.add_parser("check", help="verify pages, local links, and assets")
    check_parser.add_argument("html_directory", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
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


if __name__ == "__main__":
    sys.exit(main())
