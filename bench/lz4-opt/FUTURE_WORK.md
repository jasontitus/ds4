# Disk KV codec — future optimizations

Things we measured but did not ship in PR #186.  Kept here so we can
pick them up later without re-doing the analysis.

All numbers are Apple M1 Ultra, 8-thread codec, default 16 MiB chunks,
true cold disk reads (`sudo purge` before every individual
measurement), plain `fread` (no `F_NOCACHE`), 5-iteration medians.
Files are the three sizes used in the PR's own A/B table
(3.5 K / 17 K / 47 K tokens → 69 / 246 / 640 MiB raw).

## Baseline (what ships in PR #186)

LZ4 fast mode (`LZ4_compress_default`, equivalent to `lz4 -1`).
Batched reader: read N chunks synchronously, fork-join decompress
parallel, repeat.

|   raw | baseline read ms | codec=LZ4 ms | delta |
|------:|-----------------:|-------------:|------:|
|  69 MiB | 11.9 | 14.2 | **-16%** (slower) |
| 246 MiB | 37.4 | 38.9 | **-4%** (tied) |
| 640 MiB | 98.3 | 94.4 | **+4%** (faster) |

The PR is justified by **disk savings (~44%)**, not load speedup —
load is roughly net-zero across realistic sizes.

## Optimization 1 — lz4.c local opts (compress-side)

Patch: [`patches/lz4-fastcmov-skip4.patch`](patches/lz4-fastcmov-skip4.patch)

Two surgical edits to vendored `lz4.c`:

- `LZ4_skipTrigger` 6 → 4 (faster ramp through incompressible runs)
- Branchless safe-address in the match-candidate check (zstd PR #4165
  idiom)

Effect on real raw KV bytes, 16 MiB chunks, 8 threads:

|              | baseline | with patch |   Δ   | ratio |
|--------------|---------:|-----------:|------:|------:|
| compress C   | 3441 MB/s | 3768 MB/s | **+9.5%** | 1.808 → 1.816 |
| decompress D | 22697 MB/s | 22661 MB/s | flat | — |

Passes 1080-case lz4 verify + round-trips every real `.kv` file we
have. Doesn't help load (only compress side). Slightly better
compression ratio because the faster skip ramp leaves the hash
table less polluted between incompressible→compressible runs in
real KV data.

Why not ship: scope creep (lz4 inner-loop opts unrelated to the
streaming codec), modest absolute win, and the codec is save-bound
on save and disk-bound on load — `LZ4_compress_default` itself
isn't a hot spot.

## Optimization 2 — pipelined reader thread (load-side)

Patch: [`patches/kv-reader-thread-refactor.patch`](patches/kv-reader-thread-refactor.patch)

Replace the synchronous `kv_lz4_reader_fill_batch` (read N → fork-join
N → return batch) with a separate reader thread that streams chunks
into a ring of `batch_cap` slots while a per-chunk worker thread
decompresses each.  Consumer (cookie read callback) waits on the
next-in-order slot to be `READY`.

Effect, same data and methodology as above:

|   raw | baseline read | batched LZ4 | threaded LZ4 |
|------:|--------------:|------------:|-------------:|
|  69 MiB |  11.9 ms |  14.2 ms (-16%) |  13.8 ms (-14%) |
| 246 MiB |  37.4 ms |  38.9 ms (-4%) |  **35.4 ms (+6%)** |
| 640 MiB |  98.3 ms |  94.4 ms (+4%) |  **70.9 ms (+39%)** |

So the threading change makes the codec a clear load-time win on
long-context cache files (47 K tokens: **+39% faster than reading
uncompressed**), still a small regression on tiny caches because
the decompress cost is fundamental at that scale.

Why not ship: +150 lines of producer/consumer concurrency code
(mutex + two condvars + per-slot state machine), real test surface
on a 3-state slot machine; cleaner as a separate PR with its own
review focus.  Validated correctness: `./ds4_test` passes the
`server:` suite with this patch applied (same single pre-existing
`logprob-vectors` failure as on `kv-cache-lz4` HEAD without the
patch).

## Optimization 3 — adaptive no-compress for tiny caches

Not implemented; ~10 lines.

`kv_lz4_writer_open` would short-circuit to `codec=NONE` if the
expected uncompressed payload is below some threshold
(e.g. 32 MiB).  Tiny caches (3.5 K-token prompts and below) regress
~16% on load because decompress overhead exceeds the savings from
smaller disk reads at that size.  Skipping compression for those
recovers the 16% AND has the codec match the PR's headline disk-savings
claim only on caches large enough to actually benefit.

Open question: the writer doesn't always know the total payload size
upfront (the engine streams into the cookie).  Either (a) buffer the
first N bytes and switch to no-compress if EOF arrives before
threshold, or (b) introduce a hint argument when opening the writer.

## Optimization 4 — HC sidecar (encoder change)

Already covered in the PR's "Future HC via out-of-band recompressor"
section.  HC-encoded files (with the same on-disk format byte set to
LZ4, since the format does not distinguish levels) decompress
~30 % faster per `lz4 -b1` upstream reference numbers, on top of any
reader-thread refactor.  Projection — not measured because we don't
have HC-encoded ds4 files yet:

|   raw | -1 batched (today) | -1 threaded | HC batched | HC threaded |
|------:|-------------------:|------------:|-----------:|------------:|
| 640 MiB | 94 ms (+4%)       | 71 ms (+39%) | ~72 ms (+36%) | ~55 ms (+79%) |

Compression takes 9 MB/s single-thread / 68 MB/s 8-thread → too slow
for inline save on a fresh cache, fine for a sidecar that re-encodes
settled files during idle.  No format change needed; the chunked v2
layout already accommodates HC blocks.

## What we did not finish investigating

- **Decompression-side LZ4 inner-loop opts on real KV bytes.**
  `dec_tbl_replicate`, `dec_varlen_neon`, `dec_wildcopy_neon` all
  measured flat on the raw cache files we have (±0.2 % of baseline
  at 22.7 GB/s).  Possibly the compiler already emits NEON for the
  hot wildcopy loops; possibly decompress is memory-bandwidth-bound
  at our chunk size.  Worth a separate profiling pass before
  attempting more opts here.
- **Persistent worker pool** for very small caches.  Would save
  the ~5 ms of `pthread_create` overhead on those, but doesn't
  change the fundamental "decompress + read smaller file > read
  larger file at NVMe speed" math, so it would not turn the tiny-file
  regression into a win on its own.

## How to revisit

1. `cd bench/lz4-opt && make` builds the framework binaries
   (variant matrix lives under `build/<variant>/`).
2. The standalone load benches that produced the numbers above:
   `cc -O2 -I.. load_one.c ../lz4.c -o /tmp/load_one -lpthread`
   then `scripts/cold-load-ab.sh 5 RAW KV ...`.
3. Patches in `patches/` apply cleanly to `kv-cache-lz4` at
   commit `9442247` (the PR's tip after the
   `2026-05-26 rebase to original` cleanup).
