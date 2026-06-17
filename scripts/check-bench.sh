#!/usr/bin/env bash
# scripts/check-bench.sh — Run the Aria benchmark suite, parse percentile
# output, and fail if any P99 metric exceeds the ceiling pinned in
# benchmark/thresholds.json.
#
# Usage:
#   scripts/check-bench.sh                       # auto-discover build dir
#   scripts/check-bench.sh /path/to/build        # explicit build dir
#
# Environment overrides:
#   ARIA_BENCH_DIR        directory containing the aria_bench_* binaries.
#                         Default: <build>/bin (matches Aria's
#                         CMAKE_RUNTIME_OUTPUT_DIRECTORY). For legacy
#                         per-target output layouts, <build>/benchmark
#                         is also probed automatically.
#   ARIA_BENCH_THRESHOLDS path to thresholds.json
#                         (default: <repo>/benchmark/thresholds.json)
#   ARIA_BENCH_HOST       host key under thresholds.json:.hosts. Defaults
#                         to "<os>-<arch>" computed from `uname` (e.g.
#                         macos-arm64, linux-x86_64). When the key is
#                         absent the script falls back to the top-level
#                         .metrics block. Set explicitly to bypass auto-
#                         detection (e.g. when running on a CI runner
#                         that should reuse a different host's baseline).
#   GITHUB_STEP_SUMMARY   when set (CI), this script appends a Markdown
#                         table summarizing every metric vs. its
#                         ceiling, so the result is visible on the
#                         GitHub Actions run page without diving into
#                         the raw log.
#
# Exit codes:
#   0  every measured P99 was within budget
#   1  one or more metrics regressed past their ceiling
#   2  setup error (missing binary, malformed thresholds, etc.)
#
# Output format contract (must stay in sync with benchmark/bench_common.hpp):
#   row_pct() prints lines starting with "P " followed by a name field
#   (~52 chars), then "mean=… p50=… p95=… p99=NNN.Nns".

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"

# ── Build dir resolution ────────────────────────────────────────────────
# Prefer an explicit CLI argument; otherwise auto-discover. The repo
# uses a multi-flavor layout (build/flavors/{release,debug,http-no-tls})
# rather than a single build/, so a hard-coded default is wrong more
# often than not. Auto-discovery picks the first flavor that has
# benchmark binaries built; release wins ties.
discover_build_dir() {
    local b
    for b in \
        "${repo_root}/build/flavors/release" \
        "${repo_root}/build/flavors/debug" \
        "${repo_root}/build/flavors/http-no-tls" \
        "${repo_root}/build"; do
        if [[ -d "${b}" ]] && \
           ls "${b}/bin/aria_bench_iproperty" \
              "${b}/benchmark/aria_bench_iproperty" >/dev/null 2>&1; then
            printf '%s\n' "${b}"
            return 0
        fi
        # Looser check: any aria_bench_* binary anywhere under the dir.
        if [[ -d "${b}" ]] && \
           [[ -n "$(find "${b}" -maxdepth 4 -type f -name 'aria_bench_*' -print -quit 2>/dev/null)" ]]; then
            printf '%s\n' "${b}"
            return 0
        fi
    done
    return 1
}

# ── Argument parsing ────────────────────────────────────────────────────
# Accepted forms (any order):
#   scripts/check-bench.sh                       # auto build dir, 1 run
#   scripts/check-bench.sh /path/to/build        # explicit build dir
#   scripts/check-bench.sh --runs 5              # auto build dir, best-of-5
#   scripts/check-bench.sh /path/to/build --runs 3
#
# `--runs N` runs the full suite N times and keeps the best (lowest) P99
# per metric — useful on a developer machine that has other workloads
# contending for CPU/cache. Default 1.
build_dir=""
runs=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs)
            shift
            [[ $# -ge 1 ]] || { echo "error: --runs requires a number" >&2; exit 2; }
            runs="$1"
            shift
            ;;
        --runs=*)
            runs="${1#--runs=}"
            shift
            ;;
        --help|-h)
            sed -n '2,40p' "$0"
            exit 0
            ;;
        --)
            shift
            ;;
        -*)
            echo "error: unknown flag: $1" >&2
            exit 2
            ;;
        *)
            if [[ -n "${build_dir}" ]]; then
                echo "error: unexpected positional argument: $1" >&2
                exit 2
            fi
            build_dir="$1"
            shift
            ;;
    esac
done
if ! [[ "${runs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "error: --runs must be a positive integer, got: ${runs}" >&2
    exit 2
fi
if [[ -z "${build_dir}" ]]; then
    if ! build_dir="$(discover_build_dir)"; then
        echo "error: could not auto-discover a build dir with bench binaries" >&2
        echo "tried: build/flavors/{release,debug,http-no-tls}, build/" >&2
        echo "hint: pass an explicit path, e.g. scripts/check-bench.sh build/flavors/release" >&2
        exit 2
    fi
    echo "==> auto-discovered build dir: ${build_dir}"
fi

# Bench dir resolution: honour explicit override; otherwise probe the
# canonical layout (<build>/bin from CMAKE_RUNTIME_OUTPUT_DIRECTORY)
# first, fall back to the legacy <build>/benchmark layout.
if [[ -n "${ARIA_BENCH_DIR:-}" ]]; then
    bench_dir="${ARIA_BENCH_DIR}"
elif [[ -x "${build_dir}/bin/aria_bench_iproperty" ]]; then
    bench_dir="${build_dir}/bin"
elif [[ -x "${build_dir}/benchmark/aria_bench_iproperty" ]]; then
    bench_dir="${build_dir}/benchmark"
else
    bench_dir="${build_dir}/bin"  # default to canonical layout for the error message
fi

thresholds_file="${ARIA_BENCH_THRESHOLDS:-${repo_root}/benchmark/thresholds.json}"

# ── Host detection ──────────────────────────────────────────────────────
# Compose a "<os>-<arch>" key matching benchmark/thresholds.json:.hosts.
# The python step below looks up this key first, falls back to the
# top-level .metrics block when no host-specific entry exists.
detect_host_key() {
    local os arch
    case "$(uname -s)" in
        Darwin)            os="macos"   ;;
        Linux)             os="linux"   ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *)                 os="$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
    esac
    case "$(uname -m)" in
        x86_64|amd64)      arch="x86_64"  ;;
        arm64|aarch64)     arch="arm64"   ;;
        *)                 arch="$(uname -m)" ;;
    esac
    # Linux conventionally uses aarch64; macOS uses arm64. Normalize so
    # a single "<os>-arm64" works on Apple while Linux callers can pin
    # their own "linux-aarch64" entry if they prefer that convention.
    if [[ "${os}" == "linux" && "${arch}" == "arm64" ]]; then
        arch="aarch64"
    fi
    printf '%s-%s\n' "${os}" "${arch}"
}
host_key="${ARIA_BENCH_HOST:-$(detect_host_key)}"

if [[ ! -d "${bench_dir}" ]]; then
    echo "error: bench dir not found: ${bench_dir}" >&2
    echo "hint: run 'cmake --build ${build_dir} --target aria_bench_iproperty aria_bench_command aria_bench_async_command aria_bench_list aria_bench_derived_list' first" >&2
    exit 2
fi
if [[ ! -f "${thresholds_file}" ]]; then
    echo "error: thresholds file not found: ${thresholds_file}" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required to parse thresholds.json" >&2
    exit 2
fi

# The five bench targets we ship. Any new bench_*.cpp must register
# itself here AND in benchmark/CMakeLists.txt AND, if it emits a
# row_pct line, in thresholds.json.
benches=(
    aria_bench_iproperty
    aria_bench_command
    aria_bench_async_command
    aria_bench_list
    aria_bench_derived_list
)

# Capture combined bench output to a temp file so awk and the summary
# step can both consume it.
tmp_log="$(mktemp -t aria-bench.XXXXXX)"
trap 'rm -f "${tmp_log}"' EXIT

echo "==> running benchmark suite (host=${host_key}, runs=${runs})"
for ((run=1; run<=runs; run++)); do
    if (( runs > 1 )); then
        echo "  -- run ${run}/${runs}"
    fi
    for b in "${benches[@]}"; do
        bin="${bench_dir}/${b}"
        if [[ ! -x "${bin}" ]]; then
            echo "error: bench binary not built: ${bin}" >&2
            exit 2
        fi
        echo "    - ${b}"
        "${bin}" >>"${tmp_log}"
    done
done

# ── Compare measurements against thresholds ─────────────────────────────
# Use python3 to read the JSON ceilings and the bench log together.
# Stay strict: any "P " line whose label is unknown to thresholds.json
# is reported (so new benches don't silently slip through unchecked).
python3 - "${thresholds_file}" "${tmp_log}" "${host_key}" "${runs}" <<'PY'
import json, re, sys

thresholds_path, log_path, host_key, runs_str = sys.argv[1:5]
runs = int(runs_str)

with open(thresholds_path) as f:
    cfg = json.load(f)

# Resolve ceilings: prefer hosts.<host_key>.metrics, fall back to top-
# level .metrics (which acts as the generic baseline for un-pinned
# hosts). This lets a multi-platform CI matrix add per-runner ceilings
# under .hosts without forking the whole thresholds file.
hosts = cfg.get("hosts", {}) or {}
host_block = hosts.get(host_key) or {}
ceilings = host_block.get("metrics") or cfg.get("metrics") or {}
ceiling_source = (
    f"hosts.{host_key}" if host_block.get("metrics") else "metrics (generic fallback)"
)
if not ceilings:
    print(f"error: no 'metrics' object in {thresholds_path}", file=sys.stderr)
    sys.exit(2)
print(f"==> using ceilings from {ceiling_source}")

# row_pct format:
#   "P  <name padded>... mean=NNN.Nns  p50=NNN.Nns  p95=NNN.Nns  p99=NNN.Nns  (SxO)"
# Capture the trimmed name (everything between leading "P " and the
# first "mean=") and the p99 number.
line_re = re.compile(
    r"^P\s+(?P<name>.+?)\s+mean=\s*[\d.]+ns\s+p50=\s*[\d.]+ns\s+p95=\s*[\d.]+ns\s+p99=\s*(?P<p99>[\d.]+)ns"
)

# Each metric appears once per run when --runs N is used. Keep the best
# (lowest) P99 per metric — taking the worst would let local noise
# anchor the regression budget. Order is preserved by first-seen.
best_p99 = {}        # name -> lowest p99
order = []           # first-seen order of metric names

with open(log_path) as f:
    for raw in f:
        m = line_re.match(raw.rstrip("\n"))
        if not m:
            continue
        name = m.group("name").strip()
        p99 = float(m.group("p99"))
        if name not in best_p99:
            best_p99[name] = p99
            order.append(name)
        elif p99 < best_p99[name]:
            best_p99[name] = p99

results = [(name, best_p99[name], ceilings.get(name)) for name in order]
seen_names = set(order)

missing_in_log = [k for k in ceilings if k not in seen_names]

# ── Print a Markdown summary, both to stdout and to
#    $GITHUB_STEP_SUMMARY (so CI surfaces it on the run page).
out_targets = [sys.stdout]
gha = __import__("os").environ.get("GITHUB_STEP_SUMMARY")
if gha:
    out_targets.append(open(gha, "a"))

def emit(s):
    for t in out_targets:
        t.write(s + "\n")

emit("")
emit("### Aria nightly bench — P99 ceiling check")
emit("")
emit(f"Host: `{host_key}` · ceilings from `{ceiling_source}` · runs: {runs} (best P99 reported)")
emit("")
emit("| metric | p99 (ns) | ceiling (ns) | status |")
emit("|---|---:|---:|:---:|")

failures = 0
unknown = 0
for name, p99, ceiling in results:
    if ceiling is None:
        emit(f"| {name} | {p99:.1f} | _unknown_ | ⚠ unpinned |")
        unknown += 1
        continue
    ok = p99 <= ceiling
    mark = "✅" if ok else "❌"
    emit(f"| {name} | {p99:.1f} | {ceiling} | {mark} |")
    if not ok:
        failures += 1

if missing_in_log:
    emit("")
    emit("Threshold keys with no matching bench output:")
    for k in missing_in_log:
        emit(f"- `{k}`")

emit("")
if failures or unknown or missing_in_log:
    emit(f"**failures: {failures}, unpinned: {unknown}, missing: {len(missing_in_log)}**")
else:
    emit("**all metrics within budget**")

sys.exit(1 if (failures or unknown or missing_in_log) else 0)
PY
