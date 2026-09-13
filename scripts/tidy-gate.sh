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
# Regenerate the baseline after intentional cleanup, or when the pinned LLVM
# major is bumped: download the artifact, copy over the baseline, commit.
#
# The baseline is version-sensitive — per-check counts shift when clang-tidy
# gains checks or changes heuristics. The baseline records its LLVM version;
# a different running major fails until an intentional reviewed refresh.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
python3 - "$here" "${1:?usage: tidy-gate.sh <build-dir> [baseline-file]}" "${2:-$here/clang-tidy-baseline.txt}" <<'PYTHON'
import collections
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

here, build_dir, baseline = map(Path, sys.argv[1:])
repo = here.parent.resolve()
build_dir = build_dir.resolve()
findings_path = here / "tidy-findings.txt"
# Never leave the previous invocation's findings as this run's artifact.
findings_path.write_text("")

def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)

try:
    entries = json.loads((build_dir / "compile_commands.json").read_text())
    tus = sorted({str((Path(e["directory"]) / e["file"]).resolve()) for e in entries})
except (OSError, ValueError, KeyError, TypeError) as error:
    fail(f"invalid compile_commands.json: {error}")
if not tus:
    fail("compile_commands.json has no entries")
tool = shutil.which("clang-tidy")
if not tool:
    fail("clang-tidy is not on PATH")
version_result = subprocess.run([tool, "--version"], text=True, capture_output=True)
match = re.search(r"\b(\d+\.\d+\.\d+)\b", version_result.stdout)
if version_result.returncode or not match:
    fail(f"cannot read clang-tidy version: {version_result.stdout}{version_result.stderr}")
version = match[1]
print(f"clang-tidy {version} — linting {len(tus)} translation units", flush=True)

extra_args = []
if shutil.which("xcrun"):
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True)
    if sdk.returncode == 0 and sdk.stdout.strip():
        extra_args.append("--extra-arg=-isysroot" + sdk.stdout.strip())

def analyze(tu):
    result = subprocess.run([tool, "-p", str(build_dir), "--quiet", *extra_args, tu],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return tu, result.returncode, result.stdout

# Parse diagnostics from complete per-TU output, so parallel jobs cannot
# interleave lines. Tool failures are separate from baselined warnings.
warning = re.compile(r"^(.*?):(\d+):(\d+): warning: .*\[([a-zA-Z0-9.,-]+)\]$")
hard_error = re.compile(r"(?:^|\s)(?:fatal )?error:")
unique = set()
errors = []
with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 4)) as pool:
    for tu, status, output in pool.map(analyze, tus):
        if status or any(hard_error.search(line) for line in output.splitlines()):
            errors.append((tu, status, output))
        for line in output.splitlines():
            match = warning.match(line)
            if not match:
                continue
            path = Path(match[1])
            try:
                label = str(path.resolve().relative_to(repo))
            except ValueError:
                label = str(path)
            unique.add((label, match[2], match[3], match[4]))
counts = collections.Counter(f"{path}|{check}" for path, _, _, check in unique)
findings_path.write_text("".join(f"{key} {counts[key]}\n" for key in sorted(counts)))
if errors:
    for tu, status, output in errors:
        print(f"── analysis failed: {tu} (exit {status}) ──\n{output}", file=sys.stderr)
    fail(f"{len(errors)} translation units failed; results cannot be baselined")

print(f"── findings by check (total {sum(counts.values())}) ──")
by_check = collections.Counter()
for key, count in counts.items():
    by_check[key.rsplit("|", 1)[1]] += count
for check, count in by_check.most_common(15):
    print(f"{count:5} {check}")
print("full list: scripts/tidy-findings.txt")
if not baseline.is_file():
    print(f"── no baseline at {baseline} — AUDIT MODE, gate not enforced ──")
    sys.exit(0)

previous = {}
try:
    for line in baseline.read_text().splitlines():
        if line.startswith("# clang-tidy "):
            old = re.search(r"\d+\.\d+\.\d+", line)
            if old and old[0].split(".")[0] != version.split(".")[0]:
                fail(f"baseline uses clang-tidy {old[0]}, running {version}; align the major version")
        if not line.strip() or line.startswith("#"):
            continue
        key, count = line.rsplit(None, 1)
        previous[key] = int(count)
except (OSError, ValueError) as error:
    fail(f"invalid baseline: {error}")
regressions = []
for key, count in sorted(counts.items()):
    if key not in previous:
        regressions.append(f"NEW   {key} {count}")
    elif count > previous[key]:
        regressions.append(f"GREW  {key} {previous[key]} -> {count}")
if regressions:
    print("── NEW clang-tidy debt ──\n" + "\n".join(regressions))
else:
    print(f"── clean: no new debt vs baseline ({len(previous)} entries) ──")
sys.exit(1 if regressions else 0)
PYTHON
