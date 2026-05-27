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

## Why the wins look modest today, and what changes at GB scale

The cache files we have on hand top out at 640 MiB raw / 354 MiB on
disk.  Long-context production usage (ctx=400 K+ tokens) routinely
produces 2–8 GiB of raw KV state.  Two effects compound to make the
codec progressively more valuable as files grow:

1. **Fixed overheads** (pthread_create, mutex/condvar init,
   framing-header read) are roughly constant per load.  At ~5 ms
   total, that's ~50 % of an 11 ms small-file load but <1 % of a
   1 s multi-GB load.
2. **Pipelining only does real work when chunk_count > thread_count.**
   At default 16 MiB chunks and 8 threads, you need >128 MiB raw
   before all workers stay busy; below that, half the threads sit
   idle and the per-load wall is bounded by the LAST chunk's
   read+decompress.

### Asymptotic limits (the math)

Let `N` = uncompressed bytes, `R` = compression ratio (1.81 on real
KV), `D` = disk read throughput (~6.3 GB/s on M1 Ultra NVMe), and
`K` = aggregate parallel decompress throughput (~22.7 GB/s with 8
threads).  Ignoring fixed overhead:

- *Uncompressed read*: `N / D`
- *Batched LZ4* (serial read+decompress phases per batch):
  `N / (R·D) + N / K`
- *Threaded LZ4* (pipelined): `max(N / (R·D), N / K)`

Speedup vs uncompressed:

- batched ≈ `1 / (1/R + D/K)` = `1 / (0.55 + 0.28)` ≈ **1.20×**
- threaded ≈ `1 / max(1/R, D/K)` = `1 / 0.55` = **1.81×** — i.e.
  the disk read of the compressed file dominates and decompress is
  fully hidden under it.

So the *threaded* design asymptotes to the compression ratio itself
on large files, while the *batched* design caps at ~1.2× because the
phases serialize.

### Extrapolation table

Measured at 640 MiB; everything else from the analytic model + 15 ms
fixed overhead (should hold within ~10 %):

| raw size | baseline | batched (current PR) | threaded (refactor) |
|---:|---:|---:|---:|
|  640 MiB |   98 ms |   94 ms (+4 %, measured) |   71 ms (+39 %, measured) |
|    2 GiB | ~325 ms | ~270 ms (+20 %)           | ~195 ms (**+67 %**) |
|    8 GiB | ~1.3 s  | ~1.06 s (+22 %)           | ~0.74 s (**+77 %**) |

In other words: the small/medium load deltas reported above are the
*worst case* for the codec.  Any cache file in the GB-and-up range —
which is the actual regime for long-context conversations — gets
close to the full compression-ratio speedup with the threaded
refactor.

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

## Optimization 4 — byte-4 NEON shuffle (encoder change, drop-in)

**Single most promising idea from this whole exercise.**  Apply a
4-byte position transpose to each chunk before LZ4, undo it after
decompress.  Real KV data appears to be laid out in 4-byte aligned
blocks (quantized weights packed in 32-bit groups; bf16 pairs;
attention activations) — separating the byte streams gives LZ4 much
longer match runs.

Implementation: NEON `vld4q_u8` / `vst4q_u8` (4-way de-interleave
instructions on AArch64) make shuffle/unshuffle essentially free vs
memory bandwidth.  Scalar fallback ~3× slower.  ~50 lines total.

Measured on real raw KV bytes, 8 threads, 16 MiB chunks, NEON
byte-4 shuffle on every variant (`shuffle_mt_test.c`):

### 246 MiB raw

| level | ratio | C MB/s (agg) | D MB/s (agg) |
|---|---:|---:|---:|
| lz4-1 (today, no shuf) | 1.76× | 3441 | 22697 |
| lz4-1 + shuf | 2.10× | 5441 | 22110 |
| HC1 / HC2 + shuf | 2.30× | 3165 / 3195 | 22772 / 21539 |
| HC3 + shuf | 2.48× | 871 | 21009 |
| HC6 + shuf | 2.65× | 389 | 23192 |
| HC9 + shuf | 2.71× | 124 | 22604 |

### 640 MiB raw (the 47K-token A/B file)

| level | ratio | C MB/s (agg) | D MB/s (agg) |
|---|---:|---:|---:|
| lz4-1 (today, no shuf) | 1.81× | 3441 | 22697 |
| lz4-1 + shuf | 2.16× | 5911 | 25031 |
| **HC1 / HC2 + shuf** | **2.39×** | **3341 / 3317** | **25004 / 24482** |
| HC3 + shuf | 2.58× | 943 | 22497 |
| HC6 + shuf | 2.77× | 409 | 24731 |
| HC9 + shuf | 2.84× | 129 | 25280 |

Notes:

- **HC1 ≡ HC2** — LZ4's HC encoder clamps level <3 internally; both
  produce byte-identical output.  Effectively "HC at level 1" is a
  distinct operating point worth knowing about.
- **Decompress is memory-bandwidth-bound at every level** (~22–25
  GB/s aggregate), so encoding choice doesn't affect load time.
  Only encode time changes.
- **HC9 → HC12 buys ~+0.07× ratio for ~5× slower compress** — HC9
  is the practical max useful level; HC12 isn't worth shipping.

### Save blocks generation — what's actually inline-able today

`ds4_kvstore_store_live_prefix_text` is synchronous on the worker
thread that also drives generation.  Two save paths exist:

**Cold save** (between stable-prefix prefill and full prefill, before
the first token): the user is already waiting for prefill anyway.
Save time as fraction of the cold wait, for the 47K-token case
(~225 s prefill total):

| variant | save time | % of cold wait |
|---|---:|---:|
| today (lz4-1, no shuf) | 0.36 s | 0.16% |
| HC1 + shuf | 0.19 s | 0.08% |
| HC3 + shuf | 0.71 s | 0.32% |
| HC6 + shuf | 1.6 s | 0.71% |
| HC9 + shuf | 5.2 s | 2.31% |

All inline-able on cold save — user notices ~zero difference even at
HC9.

**Continued save** (fires every ~10 K tokens during a long
conversation): synchronous on the worker, so it stalls generation
for the save duration.  At 20 tps users expect a token every 50 ms,
so save time directly maps to "missed token slots":

| variant | save time | missed token slots |
|---|---:|---:|
| today | 359 ms | ~7 (noticeable hiccup) |
| HC1 + shuf | ~190 ms | **~4 (smaller hiccup than today)** |
| HC3 + shuf | 710 ms | ~14 (worse than today) |
| HC6 + shuf | 1.6 s | ~32 (multi-second stall) |
| HC9 + shuf | 5.2 s | ~100 (bad) |

So HC3 and above are not OK for continued saves unless we first make
the continued-save path **async** — snapshot the live KV state at
the save trigger (~640 MiB memcpy ≈ 80 ms on M1 Ultra memory
bandwidth, one-shot), then save streams from the snapshot in the
background while generation continues uninterrupted.  ~50 lines in
`ds4_kvstore.c`, requires holding 2× the chunk batch in RAM during
save.

### Recommended sequence — go to HC1 + shuf

HC1 is the only operating point that's a **strict upgrade** to the
current codec across both save paths AND load:

| metric | today (lz4-1, no shuf) | HC1 + byte-4 shuf | delta |
|---|---:|---:|---:|
| compression ratio | 1.81× | 2.39× | **+32% smaller files** |
| compress agg @ 8 thr | 3441 MB/s | 3341 MB/s | −3% (within noise) |
| decompress agg @ 8 thr | 22697 MB/s | 25004 MB/s | +10% |
| inline cold-save cost | 0.36 s | 0.19 s | **faster** |
| continued-save hiccup | ~360 ms | ~190 ms | **smaller hiccup** |

Implementation plan:

1. **Link `lz4hc.c` into ds4** (single line in `Makefile`; currently
   only `lz4.c` is linked).
2. **Add byte-4 NEON shuffle helpers** in `ds4_kvstore.c` —
   `shuffle_byte4_neon` / `unshuffle_byte4_neon` (with scalar
   fallback for non-AArch64 builds).  ~50 lines.
3. **Swap `LZ4_compress_default` for `LZ4_compress_HC(..., 1)` in
   `kv_lz4_compress_worker`**, and call the shuffle helper on the
   input first.
4. **Mirror on the reader**: call the unshuffle helper after
   `LZ4_decompress_safe` in `kv_lz4_async_decompress_worker`.
5. **On-disk format unchanged** — codec byte stays `LZ4`; the
   reader doesn't need to know what level encoded it (the bytes
   decompress the same way).  Shuffle/unshuffle is just an
   extra ~50-line transform on both sides.

Estimated total diff: ~120 lines in `ds4_kvstore.c` + 1 line in
`Makefile`.  No format change, no migration needed (old files load
unchanged because they were lz4-1 + no-shuffle, which is what
unshuffle-with-zero-effect produces if codec byte signals "no
shuffle").

If we want to expose this without breaking format compatibility, the
cleanest knob is to **reuse a reserved bit in the header** to signal
"this file was byte-4 shuffled before lz4."  v1 files (current
codec) have that bit as 0 and round-trip with no unshuffle pass.

### Larger savings (not pursued, parked for later)

| variant | extra disk savings vs today | inline? |
|---|---:|---|
| `HC3 + shuf` | +42% | cold-save only (continued stall too long) |
| `HC6 + shuf` | +53% | sidecar only |
| `HC9 + shuf` | +57% | sidecar only |

These are open routes once `HC1 + shuf` ships and we know the format
extension works.  Going past HC1 requires either bigger user-visible
saves or async continued-save.

## Optimization 5 — HC sidecar (encoder change)

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
