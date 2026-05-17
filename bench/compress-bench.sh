#!/usr/bin/env bash
# Reproduce the zstd/lz4 compress+decompress benchmarks on KV-cache-style files.
# Usage:
#   ./compress-bench.sh small.kv medium.kv large.kv
# or with no args, scans the current directory and picks min/median/max by size.
#
# Requires:
#   - zstd >= 1.5  (homebrew: brew install zstd)
#   - lz4  >= 1.10 (homebrew: brew install lz4)   # needed for -T0 MT compression
#   - python3      (just for time stamps)
#
# Originals are never written to. Compressed artifacts go to ./_compress-bench-tmp/.

set -e -o pipefail

# Return version "X.Y.Z" of the binary at $1, or empty string.
bin_version() {
    "$1" --version 2>&1 | head -1 | grep -oE 'v?[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 | tr -d v
}

# Numeric compare: returns 0 if $1 >= $2 (both "X.Y.Z").
version_ge() {
    awk -v c="$1" -v m="$2" 'BEGIN{
        split(c,ca,"."); split(m,ma,".")
        for (i=1;i<=3;i++){
            ca[i]+=0; ma[i]+=0
            if (ca[i] > ma[i]) exit 0
            if (ca[i] < ma[i]) exit 1
        }
        exit 0
    }'
}

# Search common locations (homebrew first) for a binary that meets the min version.
# Falls back to whatever is on PATH. Prints the chosen absolute path.
find_tool() {
    local name="$1" min="$2" cand v best=""
    for cand in /opt/homebrew/bin/$name /usr/local/bin/$name /opt/local/bin/$name; do
        [ -x "$cand" ] || continue
        v=$(bin_version "$cand")
        if [ -n "$v" ] && version_ge "$v" "$min"; then echo "$cand"; return 0; fi
    done
    if cand=$(command -v "$name"); then
        v=$(bin_version "$cand")
        if [ -n "$v" ] && version_ge "$v" "$min"; then echo "$cand"; return 0; fi
        best="$cand ($v)"
    fi
    echo "ERROR: need $name >= $min; ${best:-not found on PATH}.  Install with: brew install $name" >&2
    return 1
}

ZSTD=$(find_tool zstd 1.5.0) || exit 1
LZ4=$(find_tool  lz4  1.10.0) || exit 1

# Pick three input files: explicit args, or auto-pick min/median/max from cwd.
if [ "$#" -eq 3 ]; then
    SMALL="$1"; MED="$2"; LARGE="$3"
elif [ "$#" -eq 0 ]; then
    mapfile -t SIZES < <(find . -maxdepth 1 -type f -name "*.kv" -exec stat -f '%z %N' {} \; 2>/dev/null \
                       || find . -maxdepth 1 -type f -name "*.kv" -exec stat -c '%s %n' {} \; 2>/dev/null \
                       | sort -n)
    n=${#SIZES[@]}
    if [ "$n" -lt 3 ]; then echo "Need at least 3 *.kv files in cwd, or pass 3 paths" >&2; exit 1; fi
    SMALL=$(echo "${SIZES[0]}"          | awk '{print $2}')
    MED=$(echo  "${SIZES[$((n/2))]}"    | awk '{print $2}')
    LARGE=$(echo "${SIZES[$((n-1))]}"   | awk '{print $2}')
else
    echo "Usage: $0 [small.kv medium.kv large.kv]" >&2; exit 1
fi
for f in "$SMALL" "$MED" "$LARGE"; do [ -r "$f" ] || { echo "missing: $f" >&2; exit 1; }; done

stat_size() { stat -f '%z' "$1" 2>/dev/null || stat -c '%s' "$1"; }
now()       { python3 -c 'import time; print(f"{time.time():.4f}")'; }
mb()        { python3 -c "import sys; print(f'{int(sys.argv[1])/1048576:.1f}')" "$1"; }

TMP="./_compress-bench-tmp"
rm -rf "$TMP"; mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# --- system info ----------------------------------------------------------
echo "=== System ==="
if [ "$(uname)" = "Darwin" ]; then
    sysctl -n machdep.cpu.brand_string 2>/dev/null
    printf "CPU cores: %s (perf=%s, eff=%s)\n" \
        "$(sysctl -n hw.ncpu)" \
        "$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo \?)" \
        "$(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo 0)"
    printf "RAM: %s GB\n" "$(($(sysctl -n hw.memsize) / 1024 / 1024 / 1024))"
else
    grep -m1 'model name' /proc/cpuinfo 2>/dev/null
    printf "CPU cores: %s\n" "$(nproc)"
    awk '/MemTotal/ {printf "RAM: %.0f GB\n", $2/1024/1024}' /proc/meminfo
fi
echo "$ZSTD:" "$("$ZSTD" --version 2>&1 | head -1)"
echo "$LZ4: "  "$("$LZ4"  --version 2>&1 | head -1)"
echo
label_for() {
    case "$1" in
        "$SMALL") echo small ;;
        "$MED")   echo medium ;;
        *)        echo large ;;
    esac
}

echo "=== Inputs ==="
for f in "$SMALL" "$MED" "$LARGE"; do
    printf "%-9s %8s MB  %s\n" "$(label_for "$f")" "$(mb "$(stat_size "$f")")" "$f"
done
echo

# Strip the in-place CR progress, then emit one line per benchmarked level
# pairing the most recent "ratio + compress + decompress" line with the level
# marker that follows it.  Works for both zstd -b and lz4 -b output.
filter_bench() {
    tr '\r' '\n' | awk '
        # Capture progress lines that include both compress and decompress speeds
        $0 ~ /\(x?[0-9]+\.[0-9]+\)?,[[:space:]]*[0-9]+\.[0-9]+ MB\/s,[[:space:]]*[0-9]+\.[0-9]+ MB\/s/ {
            last_data = $0
        }
        # Bare "N#" line marking the end of level N
        $0 ~ /^[[:space:]]*[0-9]+#[[:space:]]*$/ {
            lev = $0
            sub(/^[[:space:]]+/, "", lev); sub(/#.*$/, "", lev)
            if (last_data != "") {
                printf "L%-3s  %s\n", lev, last_data
            } else {
                printf "L%-3s  (no data captured)\n", lev
            }
            last_data = ""
        }
    '
}

# --- in-memory benchmarks (zstd -b / lz4 -b) ------------------------------
# zstd -b auto-caps per-level time; safe to run on MEDIUM.
echo "=== zstd built-in benchmark on MEDIUM (compress + decompress, levels 1..9) ==="
"$ZSTD" -b1 -e9 -i3 "$MED" 2>&1 | filter_bench
echo
# lz4 -b runs to completion of one pass, and HC levels (>=9) on a 1.5 GB file
# would take ~15 minutes. Run on SMALL for the full level sweep; ratios are
# nearly the same and decompress speeds are level-independent.
echo "=== lz4  built-in benchmark on SMALL (compress + decompress, levels 1..12) ==="
"$LZ4"  -b1 -e12 -i3 "$SMALL" 2>&1 | filter_bench
echo
# (The MEDIUM lz4 sweep is omitted: lz4 -b's per-level iteration on >1 GiB
# inputs produces unstable timings on at least some levels because one pass
# already takes >>i seconds. The SMALL sweep covers the full level curve,
# and the multi-thread section below benchmarks lz4 -T0 at every interesting
# level on the LARGE file.)

# --- multi-thread compression sweep on LARGE ------------------------------
# Codec-aware: lz4 takes positional "input output"; zstd needs "-o output input".
mt_compress() {
    local codec="$1" level="$2" threads="$3" label="$4" out="$TMP/$label.bin"
    local t0 t1 in_size out_size rc=0
    in_size=$(stat_size "$LARGE")
    t0=$(now)
    case "$codec" in
        lz4)  "$LZ4"  "-${level}" "-T${threads}" -q -f "$LARGE" "$out" >/dev/null 2>&1 || rc=$? ;;
        zstd) "$ZSTD" "-${level}" "-T${threads}" -q -f -o "$out" "$LARGE" >/dev/null 2>&1 || rc=$? ;;
    esac
    t1=$(now)
    if [ "$rc" -ne 0 ] || [ ! -f "$out" ]; then
        printf "%-20s FAILED (rc=%s)\n" "$label" "$rc"
        return
    fi
    out_size=$(stat_size "$out")
    awk -v ins="$in_size" -v outs="$out_size" -v t0="$t0" -v t1="$t1" -v label="$label" \
      'BEGIN{dt=t1-t0; printf "%-20s wall=%6.2fs  in=%6.0fMB  out=%6.0fMB  ratio=%.2fx  thru=%6.0f MB/s\n", label, dt, ins/1048576, outs/1048576, ins/outs, ins/1048576/dt}'
}

echo "=== lz4 multi-thread compression on LARGE ($(mb "$(stat_size "$LARGE")") MB) ==="
cat "$LARGE" > /dev/null  # warm page cache for fair compress timing
echo "--- thread-count comparison at L1 (default fast) ---"
for T in 1 4 8 16 0; do mt_compress lz4 1 "$T" "lz4_L1_T${T}"; done
echo "--- thread-count comparison at L9 (HC) ---"
for T in 1 8 0; do mt_compress lz4 9 "$T" "lz4_L9_T${T}"; done
echo "--- -T0 across all levels 1..12 (full level sweep, fully multithreaded) ---"
for L in 1 2 3 4 5 6 7 8 9 10 11 12; do mt_compress lz4 "$L" 0 "lz4_L${L}_T0"; done
echo

echo "=== zstd multi-thread compression on LARGE ==="
echo "--- thread-count comparison at L3 ---"
for T in 1 8 0; do mt_compress zstd 3 "$T" "zstd_L3_T${T}"; done
echo "--- -T0 across selected levels ---"
for L in 1 3 9 19; do mt_compress zstd "$L" 0 "zstd_L${L}_T0"; done
echo

# --- decode speed (single-thread CLI, both codecs) ------------------------
echo "=== Decompress speed via CLI (single-thread; CLI pipe overhead ~10-20% vs in-mem bench above) ==="
decode_one() {
    local src="$1" dec
    if [ ! -f "$src" ]; then return; fi
    case "$src" in *lz4*) dec="$LZ4 -d -q -c";; *) dec="$ZSTD -d -q -c";; esac
    cat "$src" > /dev/null   # warm page cache
    local t0 t1 in_size
    t0=$(now)
    eval "$dec \"$src\"" > /dev/null 2>&1
    t1=$(now)
    in_size=$(stat_size "$LARGE")
    awk -v ins="$in_size" -v t0="$t0" -v t1="$t1" -v src="$(basename "$src")" \
      'BEGIN{dt=t1-t0; printf "  %-22s wall=%5.2fs  logical=%6.0fMB  thru=%6.0f MB/s\n", src, dt, ins/1048576, ins/1048576/dt}'
}
for s in "$TMP/lz4_L1_T0.bin" "$TMP/lz4_L9_T0.bin" "$TMP/lz4_L12_T0.bin" \
         "$TMP/zstd_L3_T0.bin" "$TMP/zstd_L9_T0.bin"; do
    decode_one "$s"
done

echo
echo "=== Done ==="
