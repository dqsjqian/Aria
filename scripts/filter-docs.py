#!/usr/bin/env python3
"""Keep repository links usable when Doxygen flattens Markdown into HTML."""

import argparse
from pathlib import Path
import re
import sys
from urllib.parse import quote, unquote, urlsplit, urlunsplit


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", default="main")
    parser.add_argument("--repository", default="https://github.com/dqsjqian/Aria")
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    text = source.read_text(encoding="utf-8")
    if source.suffix.lower() == ".md":
        root = Path(__file__).resolve().parent.parent
        text = filter_markdown(text, source, root, args.repository, args.revision)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
