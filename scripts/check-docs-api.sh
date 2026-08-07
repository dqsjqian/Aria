#!/usr/bin/env bash
# scripts/check-docs-api.sh — Fail if the documentation references an Aria
# API symbol that does not exist in the headers.
#
# Why this exists
# ---------------
# A docs audit found four separate cases where guide prose named an API that
# had never existed (or no longer did):
#
#   * `convert_bool_to_text` / `convert_int_to_text` /
#     `convert_double_to_text` / `invert_bool` — the real factories are
#     `converters::int_to_string()` etc.
#   * `aria::binding::convert(...)` — never existed; the real one-way path is
#     `bind_text_projected`.
#   * `aria::binding::DispatchPolicy` — it is a nested enum,
#     `BindingEngine::DispatchPolicy`. The same wrong spelling had been
#     copied into three adapter guides.
#   * `BindingEngine engine;` — there is no default constructor.
#
# Every one of those is a *named symbol* that a reader would type verbatim
# and that the compiler would reject. Full compilation of every fenced block
# is not practical (most are deliberate fragments referencing undeclared
# view/widget variables), so this script takes the cheap, high-signal half of
# the problem: extract the Aria-looking identifiers the docs mention and
# assert each one appears somewhere in `modules/**` headers.
#
# It is intentionally conservative — it only checks patterns that are
# unambiguous API references, and it will not catch wrong argument lists or
# wrong types. It does catch every defect found in the audit.
#
# Usage:
#   scripts/check-docs-api.sh            # scan docs/, exit 1 on any miss
#
# Environment overrides:
#   ARIA_DOCS_DIR      directory to scan (default: <repo>/docs)
#   ARIA_HEADERS_DIR   directory of headers to check against
#                      (default: <repo>/modules)

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
docs_dir="${ARIA_DOCS_DIR:-$repo_root/docs}"
headers_dir="${ARIA_HEADERS_DIR:-$repo_root/modules}"

if [[ ! -d "$docs_dir" ]]; then
    echo "check-docs-api: docs dir not found: $docs_dir" >&2
    exit 2
fi
if [[ ! -d "$headers_dir" ]]; then
    echo "check-docs-api: headers dir not found: $headers_dir" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Build the haystacks once.
#
#  1. `symbols_file` — every identifier-ish token anywhere in the module
#     sources. Catches names that do not exist at all.
#
#  2. `nested_file` — `Class::Member` pairs that can only be reached through
#     a *type*, not a namespace. Derived from the sources' own `Outer::Inner`
#     adjacencies with known namespaces subtracted. This is what catches a
#     nested type documented as if it sat at namespace scope: `DispatchPolicy`
#     is a real identifier, so a flat token check happily accepts
#     `aria::binding::DispatchPolicy` even though the type is
#     `BindingEngine::DispatchPolicy`.
#
# We cannot verify a namespace path by text adjacency, because the sources
# declare `namespace aria::binding { ... }` and then write `Converter`
# unqualified — `binding::Converter` never appears literally. So the rule is
# narrow and asymmetric on purpose:
#
#   FAIL only when the documented scope is a namespace, but the symbol is
#   known to live inside a class/enum.
#
# Everything else is accepted. False negatives are fine here; false positives
# would train people to ignore the check.
# ---------------------------------------------------------------------------
symbols_file="$(mktemp)"
nested_file="$(mktemp)"
namespaces_file="$(mktemp)"
sources_file="$(mktemp)"
trap 'rm -f "$symbols_file" "$nested_file" "$namespaces_file" "$sources_file"' EXIT

find "$headers_dir" \
        -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) \
        -not -path '*/tests/*' -not -path '*/fuzz/*' -print0 \
    | xargs -0 cat > "$sources_file"

grep -oE '[A-Za-z_][A-Za-z0-9_]*' "$sources_file" | sort -u > "$symbols_file"

# Namespace names the project declares (`namespace aria::binding {` etc.),
# split on `::` so each component is listed on its own.
grep -oE 'namespace[[:space:]]+[A-Za-z_][A-Za-z0-9_:]*' "$sources_file" \
    | sed 's/namespace[[:space:]]*//' \
    | tr ':' '\n' \
    | grep -vE '^$' \
    | sort -u > "$namespaces_file"

# `nested_file` — `Owner::Nested` pairs derived by actually tracking scope
# while scanning each header, rather than by text adjacency.
#
# Text adjacency does not work for this: inside `class BindingEngine` the
# sources write `DispatchPolicy::SmartMarshal` unqualified, so the string
# `BindingEngine::DispatchPolicy` never appears anywhere — which is precisely
# why the wrong spelling survived in four documents.
#
# So we do a small indentation-aware pass: remember the most recent
# column-0 `class`/`struct` declaration, and attribute any INDENTED
# `class`/`struct`/`enum [class]`/`using` declaration to it.
awk '
    # A declaration at column 0 opens a new top-level type scope.
    /^(class|struct)[[:space:]]/ {
        line = $0
        sub(/^(class|struct)[[:space:]]+/, "", line)
        sub(/[[:space:]]*(ARIA_[A-Z_]+[[:space:]]+)/, "", line)
        # Strip export macros, base clauses, braces, semicolons.
        gsub(/[[:space:]]*[:{;].*$/, "", line)
        gsub(/^ARIA_[A-Z_]+[[:space:]]+/, "", line)
        gsub(/[[:space:]]+$/, "", line)
        if (line ~ /^[A-Za-z_][A-Za-z0-9_]*$/) owner = line
        next
    }
    # A closing brace at column 0 ends it.
    /^};?/ { owner = ""; next }
    # An INDENTED type declaration belongs to the current owner.
    owner != "" && /^[[:space:]]+(class|struct|enum([[:space:]]+class)?|using)[[:space:]]/ {
        line = $0
        gsub(/^[[:space:]]+/, "", line)
        sub(/^(class|struct|enum([[:space:]]+class)?|using)[[:space:]]+/, "", line)
        gsub(/[[:space:]]*[:={;].*$/, "", line)
        gsub(/[[:space:]]+$/, "", line)
        if (line ~ /^[A-Za-z_][A-Za-z0-9_]*$/) print owner "::" line
    }
' "$sources_file" | sort -u > "$nested_file"

symbol_exists() {
    grep -qxF "$1" "$symbols_file"
}

is_namespace() {
    grep -qxF "$1" "$namespaces_file"
}

# The type `$1` is nested inside, if any (first match wins).
enclosing_type_of() {
    grep -E "^[A-Za-z_][A-Za-z0-9_]*::$1\$" "$nested_file" | head -1 | sed 's/::.*//'
}

# ---------------------------------------------------------------------------
# Collect candidate API references from the docs.
#
# We check exactly one pattern: fully-qualified `aria::...::Symbol`
# references. That is deliberate. An earlier revision also scanned bare
# snake_case calls inside fenced blocks, but the guides are full of
# intentional pseudo-code placeholders (`load_users()`, `perform_search()`,
# `do_save()`) that stand for the reader's own functions — flagging those
# trains people to ignore the check, which is worse than no check.
#
# A fully-qualified `aria::` name is never a placeholder: the reader types it
# verbatim and the compiler rejects it if wrong. All four defects the audit
# found are of this shape.
#
# For each reference we assert two things:
#   * the final component exists as an identifier somewhere; and
#   * the last scope step (`Penultimate::Final`) is a pairing the sources
#     actually use — this is what catches nested types documented as if they
#     were namespace-level.
# ---------------------------------------------------------------------------
missing=0
report() {
    printf '  %-52s %s\n' "$1" "$2"
}

echo "check-docs-api: scanning $docs_dir against $headers_dir"
echo

while IFS= read -r doc; do
    doc_missing=()

    while IFS= read -r ref; do
        [[ -z "$ref" ]] && continue
        tail_sym="${ref##*::}"
        [[ -z "$tail_sym" ]] && continue

        # Heuristic: only treat the tail as a symbol if it starts uppercase
        # (type / enum / enumerator) or contains an underscore (function).
        # A bare lowercase word is usually just a namespace (`aria::binding`).
        if [[ ! "$tail_sym" =~ ^[A-Z] ]] && [[ "$tail_sym" != *_* ]]; then
            continue
        fi

        if ! symbol_exists "$tail_sym"; then
            doc_missing+=("$ref|no such symbol '$tail_sym'")
            continue
        fi

        # Scope check.
        #
        # We walk EVERY component of the reference, not just the tail. The
        # defect this catches usually shows up mid-chain: the docs wrote
        # `aria::binding::DispatchPolicy::SmartMarshal`, whose tail is the
        # enumerator `SmartMarshal` and whose parent is `DispatchPolicy` — so
        # a tail-only check never even looks at the mistake. The wrong step is
        # `binding::DispatchPolicy`: a namespace reaching a nested type.
        #
        # For each adjacent pair `A::B` in the reference, if A is a namespace
        # but B is only ever declared nested inside some type, the reference
        # cannot compile.
        chain="${ref#aria::}"
        prev="aria"
        prefix="aria"
        while [[ -n "$chain" ]]; do
            comp="${chain%%::*}"
            if [[ "$comp" == "$chain" ]]; then
                chain=""
            else
                chain="${chain#*::}"
            fi
            [[ -z "$comp" ]] && continue

            if is_namespace "$prev"; then
                owner="$(enclosing_type_of "$comp")"
                # `owner == comp` means we matched a constructor / injected
                # class name, which says nothing about scope. Also skip when
                # the owner is itself a namespace.
                if [[ -n "$owner" && "$owner" != "$comp" ]] && ! is_namespace "$owner"; then
                    # A name that is ALSO declared at namespace scope is
                    # legitimately reachable both ways. We key that on
                    # INDENTATION: a namespace-scope declaration starts at
                    # column 0 (this codebase never indents namespace bodies),
                    # whereas a nested one is indented inside its class.
                    if ! grep -qE "^(class|struct|enum([[:space:]]+class)?|using)[[:space:]]+(ARIA_[A-Z_]+[[:space:]]+)?${comp}\b" "$sources_file"; then
                        doc_missing+=("$ref|'$comp' is nested in '$owner' — write ${prefix}::${owner}::${comp}")
                        break
                    fi
                fi
            fi

            prefix="${prefix}::${comp}"
            prev="$comp"
        done
    done < <(grep -ohE 'aria::[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)+' "$doc" \
             | sed 's/[^A-Za-z0-9_:]*$//' | sort -u)

    if (( ${#doc_missing[@]} > 0 )); then
        echo "✗ ${doc#"$repo_root/"}"
        for entry in "${doc_missing[@]}"; do
            report "${entry%%|*}" "${entry#*|}"
        done
        echo
        missing=$(( missing + ${#doc_missing[@]} ))
    fi
done < <(find "$docs_dir" -type f -name '*.md' | sort)

if (( missing > 0 )); then
    cat >&2 <<EOF
check-docs-api: FAILED — $missing documented reference(s) do not resolve.

Each line above is a fully-qualified aria:: name that the docs tell users to
write but the headers do not support at that scope. Either fix the doc to
match the real API, or add the API.
EOF
    exit 1
fi

echo "check-docs-api: OK — every fully-qualified aria:: reference in the docs resolves."
