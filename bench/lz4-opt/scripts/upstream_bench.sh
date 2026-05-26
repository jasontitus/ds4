#!/bin/bash
# Build the patched-upstream-lz4 binary for each opt combo and run lz4 -b on
# the synthetic corpora the framework already generates. This is the
# independent-methodology cross-check the user asked for: PR #186 cites
# `lz4 -b` numbers, so what shows up here is what would land in their PR
# notes if we proposed an opt upstream.
set -euo pipefail

ROOT="/Users/jasontitus/experiments/ds4/bench/lz4-opt"
UPSTREAM_SRC="/tmp/lz4-upstream"
WORK="/tmp/lz4-upstream-grid"
OUT="$ROOT/results/upstream-$(date +%Y%m%d-%H%M%S).md"

# Test corpora (already dumped earlier; redump if missing).
mkdir -p /tmp
for c in "q4:67108864" "kvcache:67108864" "mixed:67108864" "repetitive:16777216" "ascii:16777216"; do
    type="${c%:*}"; sz="${c#*:}"
    f="/tmp/${type}-${sz}.bin"
    [ -s "$f" ] || /tmp/dump_corpus "$type" "$sz" "$f"
done

CORPORA=(
    /tmp/q4-67108864.bin
    /tmp/kvcache-67108864.bin
    /tmp/mixed-67108864.bin
    /tmp/repetitive-16777216.bin
    /tmp/ascii-16777216.bin
)

# Opt combinations to test (label -> -D flags). Matches run_grid.py OPTS list
# plus the discovered "winning pair" so we can see if it survives.
declare -a COMBOS=(
    "baseline:"
    "fast_cmov:-DLZ4_OPT_FAST_CMOV=1"
    "fast_prefetch_ht:-DLZ4_OPT_FAST_PREFETCH_HT=1"
    "ht_page_align:-DLZ4_OPT_HT_PAGE_ALIGN=1"
    "fast_skip5:-DLZ4_OPT_FAST_SKIP5=1"
    "fast_skip4:-DLZ4_OPT_FAST_SKIP4=1"
    "fastcmov_skip5:-DLZ4_OPT_FAST_CMOV=1 -DLZ4_OPT_FAST_SKIP5=1"
    "fastcmov_skip4:-DLZ4_OPT_FAST_CMOV=1 -DLZ4_OPT_FAST_SKIP4=1"
    "hc_prefetch:-DLZ4_OPT_HC_PREFETCH=1"
    "hc_interleave:-DLZ4_OPT_HC_INTERLEAVE=1"
    "dec_tbl_replicate:-DLZ4_OPT_DEC_TBL_REPLICATE=1"
    "dec_varlen_neon:-DLZ4_OPT_DEC_VARLEN_NEON=1"
    "dec_no_ldp:-DLZ4_OPT_DEC_NO_LDP=1"
    "dec_wildcopy_neon:-DLZ4_OPT_DEC_WILDCOPY_NEON=1"
    "winning_pair:-DLZ4_OPT_DEC_VARLEN_NEON=1 -DLZ4_OPT_DEC_WILDCOPY_NEON=1"
)

mkdir -p "$WORK"
rm -f "$OUT"
{
    echo "# Upstream \`lz4 -b\` cross-check"
    echo ""
    echo "Generated: $(date)"
    echo ""
    echo "Each opt was applied to upstream lz4 v1.10.0 and benched with the"
    echo "stock \`programs/lz4 -b1 -i5\` benchmark, at -B7 (default 4 MiB blocks)"
    echo "and -B4 (64 KiB blocks). Numbers are the best-of-run MB/s upstream"
    echo "reports for each (compress / decompress)."
    echo ""
} >> "$OUT"

for BSIZE in 7 4; do
    BLABEL=$([ $BSIZE -eq 7 ] && echo "4 MiB blocks (-B7, upstream default)" || echo "64 KiB blocks (-B4, matches our internal bench)")
    {
        echo "## $BLABEL"
        echo ""
        printf '%-22s' "variant"
        for f in "${CORPORA[@]}"; do
            base="$(basename "$f" .bin)"
            short="$(echo "$base" | cut -d- -f1)"
            printf '  %-19s' "$short C/D MB/s"
        done
        echo
        printf '%-22s' "---"
        for f in "${CORPORA[@]}"; do printf '  %-19s' "---"; done
        echo
    } >> "$OUT"

    for combo in "${COMBOS[@]}"; do
        label="${combo%%:*}"
        flags="${combo#*:}"
        dir="$WORK/$label"
        if [ ! -x "$dir/programs/lz4" ]; then
            mkdir -p "$dir"
            rsync -a --delete "$UPSTREAM_SRC/" "$dir/" 2>/dev/null
            cp "$ROOT/lz4_opts.h" "$dir/lib/lz4_opts.h"
            cp "$ROOT/lz4/lib/lz4.c" "$dir/lib/lz4.c"
            sed -i.bak 's|"../../lz4_opts.h"|"lz4_opts.h"|' "$dir/lib/lz4.c"
            (cd "$dir" && MOREFLAGS="$flags" make -j8 >/dev/null 2>&1)
        fi

        printf '%-22s' "$label" >> "$OUT"
        for f in "${CORPORA[@]}"; do
            out=$("$dir/programs/lz4" -b1 "-B$BSIZE" -i5 "$f" 2>&1 || true)
            # Pull the final compress + decompress MB/s pair from lz4 -b's
            # in-place progress output. The final line has the best-of-N values.
            final=$(echo "$out" | tr ',' '\n' | tr '\r' '\n' | grep -E '[0-9]+\.[0-9]+ MB/s' | tail -2 | tr '\n' ' ')
            c=$(echo "$final" | grep -oE '[0-9]+\.[0-9]+' | head -1)
            d=$(echo "$final" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
            printf '  %8s / %-8s' "${c:-?}" "${d:-?}" >> "$OUT"
        done
        echo >> "$OUT"
    done
    echo "" >> "$OUT"
done

echo "Wrote $OUT"
