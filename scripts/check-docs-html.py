#!/usr/bin/env python3
"""Check generated API pages, local links, and assets before publication."""

import argparse
from html.parser import HTMLParser
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("html_directory", type=Path)
    args = parser.parse_args()
    count, errors = check(args.html_directory.resolve())
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"check-docs-html: {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"check-docs-html: {count} HTML pages, {len(PUBLIC_PAGES)} public API samples, local links and assets OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
