#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# tidy-gate.sh — clang-tidy baseline gate
#
# Runs clang-tidy over every translation unit in compile_commands.json
# (headers are covered via HeaderFilterRegex in .clang-tidy), normalizes
# findings to "path|check count" and compares against a checked-in
# baseline. Fails only on NEW debt: a (file, check) pair the baseline has
# never seen, or a count that grew. Shrinking/disappearing findings pass.
#
# Any "error:" finding is a hard failure regardless of baseline — an error
# means a TU did not analyze properly (bad flags, missing include).
#
# Usage:
#   scripts/tidy-gate.sh <build-dir> [baseline-file]
#
#   <build-dir>     cmake build dir containing compile_commands.json
#   [baseline-file] defaults to scripts/clang-tidy-baseline.txt. Missing
#                   baseline → audit mode: print + write findings, exit 0.
#
# Every run writes scripts/tidy-findings.txt (CI uploads it as artifact).
# Regenerate the baseline after intentional cleanup, or when a brew LLVM
# bump adds checks: download the artifact, copy over the baseline, commit.
set -euo pipefail

repo="$(git rev-parse --show-toplevel)"
here="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:?usage: tidy-gate.sh <build-dir> [baseline-file]}"
baseline="${2:-$here/clang-tidy-baseline.txt}"
cc="$build_dir/compile_commands.json"
[[ -f "$cc" ]] || { echo "error: no compile_commands.json in $build_dir" >&2; exit 2; }

tus="$(sed -n 's/.*"file": "\([^"]*\)".*/\1/p' "$cc")"
n_tus="$(printf '%s\n' "$tus" | grep -c .)"
[[ "$n_tus" -gt 0 ]] || { echo "error: compile_commands.json has no entries" >&2; exit 2; }

version="$(clang-tidy --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
echo "clang-tidy $version — linting $n_tus translation units"

# compile_commands may come from a different compiler (e.g. AppleClang) than
# clang-tidy (Homebrew LLVM). AppleClang's flags carry no explicit SDK path —
# it finds one implicitly — but Homebrew clang-tidy does not, so every TU
# dies on 'cstddef' file not found. Pass the SDK explicitly; harmless when
# both sides agree.
extra_args=()
if sdk="$(xcrun --show-sdk-path 2>/dev/null)" && [[ -n "$sdk" ]]; then
  extra_args+=("--extra-arg=-isysroot$sdk")
fi

# Per-TU logs in parallel; diagnostics never interleave mid-line.
tmp="$(mktemp -d /tmp/aria-tidy.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
jobs_n="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
printf '%s\n' "$tus" | xargs -P "$jobs_n" -I'{}' sh -c \
  'clang-tidy -p "$1" --quiet '"${extra_args[*]}"' "$2" > "$3/$(printf %s "$2" | cksum | cut -d" " -f1).log" 2>&1 || true' \
  _ "$build_dir" '{}' "$tmp"
cat "$tmp"/*.log > "$tmp/all.log" || true

# A TU that failed to analyze is never baselined.
if grep -qE ':[0-9]+:[0-9]+: error:' "$tmp/all.log"; then
  echo "── clang-tidy hard errors (not baselined) ──" >&2
  grep -E ':[0-9]+:[0-9]+: error:' "$tmp/all.log" >&2
  exit 2
fi

# "path:line:col: warning: msg [check]" → dedupe exact findings across
# TUs → drop line:col (immune to line drift) → count per path|check.
grep -E ':[0-9]+:[0-9]+: warning:' "$tmp/all.log" \
  | sed -E "s#^$repo/##" \
  | sed -E 's#^([^:]+):([0-9]+):([0-9]+): warning: .*\[([a-zA-Z0-9.-]+)\]$#\1:\2:\3|\4#' \
  | grep -E '^[^:]+:[0-9]+:[0-9]+\|[a-zA-Z0-9.-]+$' \
  | sort -u \
  | sed -E 's#^([^:]+):[0-9]+:[0-9]+(\|.*)$#\1\2#' \
  | sort | uniq -c | awk '{print $2" "$1}' | sort > "$here/tidy-findings.txt"

total="$(awk '{s+=$2} END{print s+0}' "$here/tidy-findings.txt")"
echo "── findings by check (total $total) ──"
awk '{c=$1; sub(/.*\|/,"",c); n[c]+=$2} END{for(k in n) printf "%5d %s\n", n[k], k}' \
  "$here/tidy-findings.txt" | sort -rn | head -15
echo "full list: scripts/tidy-findings.txt"

if [[ ! -f "$baseline" ]]; then
  echo "── no baseline at $baseline — AUDIT MODE, gate not enforced ──"
  echo "copy tidy-findings.txt to clang-tidy-baseline.txt and commit to enforce."
  exit 0
fi

status=0
out="$(awk '
  NR==FNR { base[$1]=$2; next }
  {
    if (!($1 in base))       { print "NEW   " $0; bad=1 }
    else if ($2 > base[$1])  { print "GREW  " $1" " base[$1]" -> "$2; bad=1 }
  }
  END { exit bad }
' "$baseline" "$here/tidy-findings.txt")" || status=1

if [[ "$status" -ne 0 ]]; then
  echo "── NEW clang-tidy debt (baseline: $(wc -l < "$baseline" | tr -d ' ') entries) ──"
  printf '%s\n' "$out"
  echo "fix it, or regenerate the baseline (see header of this script)."
else
  echo "── clean: no new debt vs baseline ($(wc -l < "$baseline" | tr -d ' ') entries) ──"
fi
exit "$status"
