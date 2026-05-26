#!/usr/bin/env python3
"""LZ4 optimization grid runner.

Builds the LZ4 library N times with different optimization flags enabled,
verifies round-trip correctness for each build, runs the bench binary
across a corpus matrix, and emits both a JSONL log and a Markdown grid
of compress/decompress throughput.

Usage:
    python3 scripts/run_grid.py --mode singles
    python3 scripts/run_grid.py --mode pairs
    python3 scripts/run_grid.py --mode all
    python3 scripts/run_grid.py --mode custom \\
        --custom 'LZ4_OPT_HC_PREFETCH,LZ4_OPT_HC_INTERLEAVE' \\
                 'LZ4_OPT_FAST_CMOV'

Modes:
    singles  baseline + each opt individually
    pairs    singles + all pairs + all-on
    all      every non-empty subset (2^N combinations) — careful, N=8 = 256
    custom   baseline + the comma-separated flag sets in --custom

Results are written under results/grid-<timestamp>.{jsonl,md} relative to
the framework root.
"""
from __future__ import annotations

import argparse
import itertools
import json
import os
import subprocess
import sys
import time
from pathlib import Path

# Every opt the framework knows about. Keep in sync with lz4_opts.h.
OPTS = [
    "LZ4_OPT_FAST_CMOV",
    "LZ4_OPT_FAST_PREFETCH_HT",
    "LZ4_OPT_HT_PAGE_ALIGN",
    "LZ4_OPT_HC_PREFETCH",
    "LZ4_OPT_HC_INTERLEAVE",
    "LZ4_OPT_DEC_TBL_REPLICATE",
    "LZ4_OPT_DEC_VARLEN_NEON",
    "LZ4_OPT_DEC_NO_LDP",
]

# (label, corpus_args, block, level)
#
# Corpora are kept short labels but cover the full range we care about:
# - text/json/repetitive: classical compressible streams
# - random: incompressible worst-case
# - q4/q8/f16/bf16/kvcache: ds4-shaped data (quantized weights + activations
#   + block-structured KV cache rows)
# - mixed: a sanity blend of the first four
DEFAULT_BENCHES = [
    ("ascii-1M-blk64k",     ["--synth", "ascii:1048576"],      65536, 0),
    ("ascii-1M-blk4k",      ["--synth", "ascii:1048576"],       4096, 0),
    ("repet-1M-blk64k",     ["--synth", "repetitive:1048576"], 65536, 0),
    ("json-1M-blk64k",      ["--synth", "json:1048576"],       65536, 0),
    ("random-1M-blk64k",    ["--synth", "random:1048576"],     65536, 0),
    ("mixed-4M-blk64k",     ["--synth", "mixed:4194304"],      65536, 0),
    # ds4-shaped data
    ("q4-2M-blk64k",        ["--synth", "q4:2097152"],         65536, 0),
    ("q8-2M-blk64k",        ["--synth", "q8:2097152"],         65536, 0),
    ("f16-2M-blk64k",       ["--synth", "f16:2097152"],        65536, 0),
    ("bf16-2M-blk64k",      ["--synth", "bf16:2097152"],       65536, 0),
    ("kvcache-4M-blk64k",   ["--synth", "kvcache:4194304"],    65536, 0),
    ("kvcache-4M-blk4k",    ["--synth", "kvcache:4194304"],     4096, 0),
    # HC at level 9 to exercise the chain-walk opts
    ("ascii-1M-HC9-blk64k", ["--synth", "ascii:1048576"],      65536, 9),
    ("json-1M-HC9-blk64k",  ["--synth", "json:1048576"],       65536, 9),
    ("kvcache-2M-HC9-blk64k", ["--synth", "kvcache:2097152"],  65536, 9),
]


def variant_label(flags: list[str]) -> str:
    if not flags:
        return "baseline"
    return "+".join(sorted(f.replace("LZ4_OPT_", "").lower() for f in flags))


def build_variant(root: Path, flags: list[str], *, jobs: int, quiet: bool) -> str:
    label = variant_label(flags)
    cflags = " ".join(f"-D{f}=1" for f in flags)
    cmd = ["make", f"-j{jobs}", f"VARIANT={label}", f"OPT_CFLAGS={cflags}"]
    if quiet:
        cmd.insert(1, "-s")
    # Always start with a clean per-variant tree so stale objects can't mask
    # macro changes.
    subprocess.run(["make", "-s", f"VARIANT={label}", "clean-variant"], cwd=root, check=False)
    res = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"build failed for {label}:\n{res.stdout}\n{res.stderr}")
    return label


def verify_variant(root: Path, label: str) -> tuple[bool, str]:
    bin_path = root / "build" / label / "verify"
    res = subprocess.run([str(bin_path)], cwd=root, capture_output=True, text=True)
    return res.returncode == 0, res.stderr.strip()


def bench_variant(root: Path, label: str, benches, iters: int, warmup: int):
    bin_path = root / "build" / label / "bench"
    rows = []
    for bname, corpus_args, block, level in benches:
        cmd = [
            str(bin_path), "--json",
            "--variant", label,
            "--block", str(block),
            "--level", str(level),
            "--iters", str(iters),
            "--warmup", str(warmup),
            "--quiet",
        ] + corpus_args
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"  BENCH FAILED {label}/{bname}:\n{res.stderr}", file=sys.stderr)
            continue
        last_line = next(
            (ln for ln in reversed(res.stdout.strip().splitlines()) if ln.strip().startswith("{")),
            None,
        )
        if not last_line:
            print(f"  no JSON for {label}/{bname}\nstdout: {res.stdout!r}", file=sys.stderr)
            continue
        try:
            row = json.loads(last_line)
        except json.JSONDecodeError as e:
            print(f"  bad JSON for {label}/{bname}: {e}: {last_line!r}", file=sys.stderr)
            continue
        row["bench_label"] = bname
        rows.append(row)
    return rows


def gen_combinations(opts: list[str], mode: str):
    yield []  # baseline always first
    if mode == "singles":
        for o in opts:
            yield [o]
        return
    if mode == "pairs":
        for o in opts:
            yield [o]
        for a, b in itertools.combinations(opts, 2):
            yield [a, b]
        if opts:
            yield list(opts)
        return
    if mode == "all":
        n = len(opts)
        for k in range(1, n + 1):
            for c in itertools.combinations(opts, k):
                yield list(c)
        return


def format_grid(rows, metric: str, fmt: str = "{:>10.1f}") -> str:
    benches: list[str] = []
    seen_b = set()
    for r in rows:
        if r["bench_label"] not in seen_b:
            seen_b.add(r["bench_label"])
            benches.append(r["bench_label"])
    variants: list[str] = []
    seen_v = set()
    for r in rows:
        if r["variant"] not in seen_v:
            seen_v.add(r["variant"])
            variants.append(r["variant"])
    by = {(r["variant"], r["bench_label"]): r for r in rows}
    name_w = max((len(v) for v in variants), default=8)
    name_w = max(name_w, len("variant"))
    cell_w = 10
    header = ["variant".ljust(name_w)] + [b.center(cell_w) for b in benches]
    sep = ["-" * name_w] + ["-" * cell_w for _ in benches]
    lines = [" | ".join(header), "-+-".join(sep)]
    base = by.get(("baseline", benches[0])) if benches else None
    for v in variants:
        cells = [v.ljust(name_w)]
        for b in benches:
            r = by.get((v, b))
            if r is None:
                cells.append("".rjust(cell_w))
                continue
            val = r.get(metric)
            if isinstance(val, (int, float)):
                cells.append(fmt.format(val).rjust(cell_w))
            else:
                cells.append(str(val).rjust(cell_w))
        lines.append(" | ".join(cells))
    return "\n".join(lines)


def format_delta_grid(rows, metric: str) -> str:
    """Same as format_grid but renders % delta vs baseline per column."""
    baselines = {}
    for r in rows:
        if r["variant"] == "baseline":
            baselines[r["bench_label"]] = r.get(metric)
    benches: list[str] = []
    seen = set()
    for r in rows:
        if r["bench_label"] not in seen:
            seen.add(r["bench_label"])
            benches.append(r["bench_label"])
    variants = []
    seen = set()
    for r in rows:
        if r["variant"] not in seen:
            seen.add(r["variant"])
            variants.append(r["variant"])
    by = {(r["variant"], r["bench_label"]): r for r in rows}
    name_w = max((len(v) for v in variants), default=8)
    name_w = max(name_w, len("variant"))
    cell_w = 10
    header = ["variant".ljust(name_w)] + [b.center(cell_w) for b in benches]
    sep = ["-" * name_w] + ["-" * cell_w for _ in benches]
    lines = [" | ".join(header), "-+-".join(sep)]
    for v in variants:
        cells = [v.ljust(name_w)]
        for b in benches:
            r = by.get((v, b))
            base = baselines.get(b)
            if r is None or base is None or base == 0:
                cells.append("".rjust(cell_w))
                continue
            cur = r.get(metric)
            if not isinstance(cur, (int, float)):
                cells.append("".rjust(cell_w))
                continue
            pct = (cur - base) * 100.0 / base
            cells.append(("{:+.2f}%".format(pct)).rjust(cell_w))
        lines.append(" | ".join(cells))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["singles","pairs","all","custom"], default="singles")
    ap.add_argument("--opts", nargs="*", default=OPTS,
                    help="restrict the opt set considered (default: all known opts)")
    ap.add_argument("--custom", nargs="*", default=[],
                    help="for --mode=custom: comma-separated flag sets, e.g. 'A,B' 'C'")
    ap.add_argument("--iters", type=int, default=11)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--results", default="results")
    ap.add_argument("--skip-verify", action="store_true",
                    help="don't run the verify binary before benching (faster iteration)")
    ap.add_argument("--quiet-build", action="store_true")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    results_dir = (root / args.results)
    results_dir.mkdir(exist_ok=True)
    ts = time.strftime("%Y%m%d-%H%M%S")
    log_path = results_dir / f"grid-{ts}.jsonl"
    md_path  = results_dir / f"grid-{ts}.md"

    if args.mode == "custom":
        combos = [[]]
        for s in args.custom:
            combos.append([x for x in s.split(",") if x])
    else:
        combos = list(gen_combinations(args.opts, args.mode))

    print(f"# {len(combos)} variant(s) planned; log -> {log_path}", file=sys.stderr)
    all_rows = []
    with log_path.open("w") as logf:
        for combo in combos:
            label = variant_label(combo)
            t_start = time.time()
            print(f"\n== {label} ==", file=sys.stderr)
            try:
                build_variant(root, combo, jobs=args.jobs, quiet=args.quiet_build)
            except RuntimeError as e:
                print(str(e), file=sys.stderr)
                continue
            t_build = time.time() - t_start
            if not args.skip_verify:
                ok, msg = verify_variant(root, label)
                if not ok:
                    print(f"  VERIFY FAILED for {label}; skipping bench\n{msg}", file=sys.stderr)
                    continue
            rows = bench_variant(root, label, DEFAULT_BENCHES, args.iters, args.warmup)
            for r in rows:
                r["flags"] = combo
                r["build_seconds"] = round(t_build, 2)
                json.dump(r, logf)
                logf.write("\n")
                all_rows.append(r)
                print(f"  {r['bench_label']:24s}  C={r['compress_mbps_med']:8.1f}  "
                      f"D={r['decompress_mbps_med']:8.1f}  ratio={r['ratio']:.3f}", file=sys.stderr)

    if not all_rows:
        print("no successful runs; nothing to summarize.", file=sys.stderr)
        return

    with md_path.open("w") as f:
        f.write(f"# LZ4-opt grid ({ts})\n\n")
        f.write(f"- Mode: `{args.mode}`\n- Iterations: {args.iters} (warmup {args.warmup})\n")
        f.write(f"- Variants: {len(set(r['variant'] for r in all_rows))}\n")
        f.write(f"- Benches: {len(set(r['bench_label'] for r in all_rows))}\n\n")
        f.write("## Compress (MB/s, median)\n\n```\n")
        f.write(format_grid(all_rows, "compress_mbps_med")); f.write("\n```\n\n")
        f.write("## Compress (% vs baseline, median)\n\n```\n")
        f.write(format_delta_grid(all_rows, "compress_mbps_med")); f.write("\n```\n\n")
        f.write("## Decompress (MB/s, median)\n\n```\n")
        f.write(format_grid(all_rows, "decompress_mbps_med")); f.write("\n```\n\n")
        f.write("## Decompress (% vs baseline, median)\n\n```\n")
        f.write(format_delta_grid(all_rows, "decompress_mbps_med")); f.write("\n```\n\n")
        f.write("## Compression ratio (input/compressed)\n\n```\n")
        f.write(format_grid(all_rows, "ratio", fmt="{:>10.4f}")); f.write("\n```\n")

    print(f"\nLog:     {log_path}")
    print(f"Summary: {md_path}")


if __name__ == "__main__":
    main()
