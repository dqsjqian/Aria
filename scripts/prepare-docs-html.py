#!/usr/bin/env python3
"""Repair known Doxygen navigation defects before strict link validation."""

import argparse
from html import escape, unescape
from html.parser import HTMLParser
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit, urlunsplit


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("html_directory", type=Path)
    args = parser.parse_args()
    print(f"prepare-docs-html: repaired {prepare(args.html_directory.resolve())} Doxygen navigation links")


if __name__ == "__main__":
    main()
