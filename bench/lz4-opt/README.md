# lz4-opt — LZ4 optimization benchmarking framework

A self-contained harness for testing performance optimizations to LZ4. It:

1. Vendors upstream LZ4 (`lz4/` — currently the dev branch, 1.10.0+).
2. Defines a registry of optimization flags (`LZ4_OPT_*`) in `lz4_opts.h`.
3. Builds a separate library variant per flag combination under `build/<variant>/`.
4. Verifies round-trip correctness for every variant.
5. Benchmarks compress + decompress throughput across a synthetic corpus matrix.
6. Emits a grid of results (Markdown + JSONL) showing single- and pairwise-effects.

## Quick start

```bash
cd bench/lz4-opt
make                                         # baseline (no opts enabled)
./build/baseline/verify                      # 100+ round-trip cases
./build/baseline/bench --synth ascii:1048576 --block 65536

# Single-flag grid (baseline + one variant per known opt).
python3 scripts/run_grid.py --mode singles

# All pairs + baseline + all-on.
python3 scripts/run_grid.py --mode pairs

# Specific custom flag sets.
python3 scripts/run_grid.py --mode custom \
    --custom 'LZ4_OPT_HC_PREFETCH,LZ4_OPT_HC_INTERLEAVE' 'LZ4_OPT_FAST_CMOV'
```

Results land in `results/grid-<timestamp>.{jsonl,md}`.

## Layout

```
bench/lz4-opt/
├── Makefile         # builds VARIANT under build/<VARIANT>/
├── lz4_opts.h       # central registry of LZ4_OPT_* flags
├── bench.c          # times compress + decompress, emits JSON
├── verify.c         # exhaustive round-trip check
├── corpus.{c,h}     # synthetic corpora + file loader
├── timing.h         # CLOCK_MONOTONIC_RAW timing helper
├── scripts/
│   └── run_grid.py  # builds & runs the matrix, emits grid + JSONL
├── lz4/             # vendored upstream (do not edit casually)
├── build/           # per-variant outputs (gitignored)
└── results/         # grid logs/summaries (gitignored)
```

## How an optimization gets added

The vendored `lz4/` tree is meant to stay close to upstream. Each
experiment is a *single guarded block* in `lz4/lib/lz4.c` or `lz4hc.c`,
so we can diff a variant against upstream cleanly.

1. Add `LZ4_OPT_FOO` (default 0) in `lz4_opts.h`.
2. Append it to the banner format/args.
3. Add `"LZ4_OPT_FOO"` to the `OPTS` list in `scripts/run_grid.py`.
4. At the top of the affected LZ4 source file (just once per file),
   add `#include "../../lz4_opts.h"` if it's not already there.
5. Wrap the experimental code in `#if LZ4_OPT_FOO ... #else ... #endif`.
6. Run `python3 scripts/run_grid.py --mode singles --opts LZ4_OPT_FOO`.
7. Once correct in isolation, run pair tests:
   `python3 scripts/run_grid.py --mode pairs --opts LZ4_OPT_FOO <related-opts>`.

## Corpora

All synthetic — generated deterministically from a fixed xorshift seed so
runs are reproducible across machines:

**General shapes:**
- `random`     — incompressible, validates we don't regress on the worst case.
- `ascii`      — English-text-shaped byte frequencies.
- `repetitive` — short runs of varying length (stresses small-offset
                 RLE paths in the decode loop).
- `json`       — short repeating structural patterns; representative of
                 log/database workloads.
- `mixed`      — equal blend of the above.

**ds4-shaped data** (these matter much more than ASCII for the DS4 KV cache
workload — quantized integer streams and fp16 activations look nothing like
English text and have very different LZ4 dynamics):
- `q4`      — blocks of [fp16 scale | 16 bytes packed signed 4-bit quants],
              weighted toward small magnitudes.
- `q8`      — blocks of [fp16 scale | 32 signed 8-bit quants].
- `f16`     — small-magnitude fp16 activation-shaped values, ~10% explicit
              zeros, tight exponent cluster.
- `bf16`    — small-magnitude bf16 with exponent biased near 127 (~1.0).
- `kvcache` — block-structured rows mimicking ds4 KV layout:
              `[32 B header][1024 B Q8 KV][512 B fp16 indexer][1024 B Q4 weights][optional zero run]`.

To benchmark against a real file:
```bash
./build/<variant>/bench --corpus path/to/silesia.tar --block 65536 --iters 11
```

## Apple Silicon notes

The framework runs on macOS arm64 as well as Linux x86 / aarch64. On
Apple Silicon, the variance-control checklist is:

- Build and run from a foreground Terminal so QoS is user-interactive,
  not background.
- Plug in to AC power (frequency governor differs on battery).
- Close Chrome/Slack/etc. while measuring; thermal pressure on the
  shared L2 will swamp 1–3% effects.
- Don't pin to a specific core — Apple doesn't expose CPU affinity to
  user processes. The OS scheduler will keep a high-QoS process on a
  P-core. (`taskset` only works on Linux.)
- The framework uses `CLOCK_MONOTONIC_RAW` where available (Linux);
  on macOS this falls back to `CLOCK_MONOTONIC`, which is good enough
  at the ns granularity we measure.

The NEON-specific opts (`*_TBL_REPLICATE`, `*_VARLEN_NEON`, `*_NO_LDP`)
compile to no-ops on x86; you can still test the architecture-neutral
opts (`*_CMOV`, `*_PREFETCH*`, `*_PAGE_ALIGN`) on the x86 host before
shipping the patch over to a Mac.

## What's currently implemented

This commit ships the **framework only** — the vendored LZ4 source is
pristine, and every `LZ4_OPT_*` flag is wired up but not yet attached to
any code change. A `--mode singles` run right now should show the same
numbers across every variant. That's the baseline noise floor; any
optimization we layer in afterwards should clear it.
