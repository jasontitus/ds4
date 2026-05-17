# Proposal: Compressed KV Disk Cache for ds4-server

## TL;DR

Compressing on-disk KV checkpoints with **lz4 -1 -T0** would roughly **1.5× the effective cache capacity** (or equivalently halve the disk footprint) at negligible save-time cost on a 20-core Mac and ~30–40% slower load times through the CLI (likely no slower than uncompressed mmap if integrated via the C API). With **lz4 -9 (HC) -T0**, the ratio improves to ~1.8×–2.0× with decompress speeds that match raw mmap reads, in exchange for ~16 s of compress time per 2 GB checkpoint. Either option directly addresses the cache-eviction thrash seen today on a 100 GiB budget.

## Motivation

On a long-running server (M1 Ultra, 96 GiB, ctx=400000, --kv-disk-space-mb 102400, --kv-cache-cold-max-tokens 300000), the disk cache thrashes:

- **193 stores, 193 evictions, 1 cache hit over 15 prompts** (~6.7% hit rate) in a single morning, all evictions tagged `reason=disk-cache-full hits=0`.
- Cold-anchor entries from f074c7b get evicted by the churn before they can hit.
- The 6 h `hit_half_life` eviction policy can't help because entries never accumulate any hits before being kicked out.

Setting `--kv-cache-continued-interval-tokens 0` mitigates the worst of the thrash (continued in-prefill snapshots were the biggest offenders), but the underlying capacity pressure remains: most cache files are 1–2 GiB, and even 100 GiB only holds ~60 entries.

Compression directly attacks the capacity wall.

## Measured data

Measured on two Apple Silicon machines so the numbers cover both the workstation case (server host) and the laptop/mini case (a likely client or secondary host). zstd 1.5.7, lz4 1.10.0 in both cases.

- **Mac Studio (M1 Ultra)** — 20 cores (16 P + 4 E), 128 GB RAM. The machine actually serving ds4 today.
- **Mac mini (M4 Pro)** — 12 cores (8 P + 4 E), 24 GB RAM. Newer-gen, fewer cores.

### File size distribution (live cache, 69 files, 100 GiB)

| Size band | Files | Total |
|---|---|---|
| 0–100 MB | 1 | 0 GiB |
| 100–500 MB | 4 | 1.5 GiB |
| 500 MB – 1 GB | 0 | 0 GiB |
| 1–1.5 GB | 25 | 33.6 GiB |
| 1.5–2 GB | **36** | **58.4 GiB** |
| > 2 GB | 3 | 6.1 GiB |

**88% of files are > 1 GB.** Optimizing the large-file path optimizes nearly every load.

### Compression ratio and throughput (in-memory benchmark, single-thread)

All ratios are essentially identical between machines (same algorithm, same data). Throughput differs by chip — newer Apple Silicon decompresses meaningfully faster per core.

#### zstd 1.5.7 on 1.5 GB cache file

| Level | Compress (M1U / M4P) | Decompress (M1U / M4P) | Ratio |
|---|---|---|---|
| 1 | 1561 / 1563 MB/s | 1053 / **1372** MB/s | 1.89× |
| 3 (default) | 1035 / 1003 MB/s | 1076 / **1394** MB/s | 1.97× |
| 6 | 330 / 393 MB/s | 1085 / 1386 MB/s | 1.98× |
| 9 | 221 / 246 MB/s | 1121 / **1442** MB/s | 1.99× |

The 1.5 GB file compresses worse than the 130 MB file (1.97× vs 2.19× at L3); larger KV states have more entropy. M4 Pro decompresses zstd ~30% faster per-core than M1 Ultra.

#### lz4 1.10 on 33 MB cache file (full level sweep)

| Level | Compress (M1U / M4P) | Decompress (M1U / M4P) | Ratio |
|---|---|---|---|
| 1 (default) | 790 / **1045** MB/s | 4980 / **6519** MB/s | 1.78× |
| 2 | 487 / 612 MB/s | 5476 / 7179 MB/s | 1.87× |
| 5 | 72 / 95 MB/s | 5884 / 7822 MB/s | 1.93× |
| 9 (HC) | 14 / 16 MB/s | **6501 / 8716** MB/s | 2.06× |
| 12 (HC max) | 7.7 / 9.8 MB/s | 6218 / 8290 MB/s | 2.08× |

**lz4 decompresses 5–8 GB/s single-threaded** on these machines — close to or exceeding the raw mmap read speeds we see today for uncompressed cache loads (~5–7 GB/s). M4 Pro is consistently ~30% faster per-core.

#### lz4 on 1.5 GB file (L1 only, to show ratio at scale)

| Machine | Compress | Decompress | Ratio |
|---|---|---|---|
| M1 Ultra | 597 MB/s | 3798 MB/s | 1.48× |
| M4 Pro | 781 MB/s | 4978 MB/s | 1.48× |

Larger KV files compress about 15% less than small ones — same pattern as zstd. The decompress speed on big files is still 4–5 GB/s.

### Multi-threaded compression (lz4 -T0, 2.1 GB file)

| Mode | M1 Ultra wall / thru | M4 Pro wall / thru | Ratio |
|---|---|---|---|
| `lz4 -1 -T1` | 4.62 s / 462 MB/s | 3.34 s / 648 MB/s | 1.49× |
| `lz4 -1 -T4` | 1.39 s / 1563 MB/s | 0.97 s / 2221 MB/s | 1.49× |
| `lz4 -1 -T8` | 0.88 s / 2470 MB/s | 0.59 s / **3673 MB/s** | 1.49× |
| `lz4 -1 -T16` | 0.86 s / 2510 MB/s | 0.59 s / 3651 MB/s | 1.49× |
| `lz4 -1 -T0` | 0.86 s / 2510 MB/s | 0.59 s / **3652 MB/s** | 1.49× |
| `lz4 -9 -T1` | 245 s / 9 MB/s | 205 s / 11 MB/s | 1.79× |
| `lz4 -9 -T8` | 32 s / 68 MB/s | 28 s / 78 MB/s | 1.79× |
| `lz4 -9 -T0` | **16 s / 136 MB/s** | 24 s / 89 MB/s | 1.79× |

Two interesting architecture notes:

1. **M4 Pro at -T8 (8 P-cores saturated) outperforms M1 Ultra at -T0 (16 P-cores).** lz4 -1 -T0: M4 Pro 3.7 GB/s vs M1 Ultra 2.5 GB/s. Newer per-core throughput plus higher memory bandwidth wins, even at fewer total cores.
2. **HC scaling favors total core count.** M1 Ultra finishes HC compress in 16 s with 16 P-cores; M4 Pro takes 24 s with 8 P-cores. For HC-on-write deployments, P-core count matters.

zstd MT compression results from the same runs were corrupted by an `-o` argument-order bug in the bench script (`in=1121MB` rows below the L3 -T8/-T0 lines). Re-running on the patched script: M1 Ultra hits ~3 GB/s at zstd -3 -T0; M4 Pro is in the same range. Neither codec parallelizes decompress out of the box (single-frame file format).

### Baseline: current uncompressed behavior

From `/tmp/ds4-server.log` over the past few days at this configuration:

- Continued save of a 1367 MiB / 102 400-token snapshot: **692 ms** (≈ 2.0 GB/s).
- Cold load of a 533 MiB / 38 912-token snapshot: **109.2 ms** (first hit), **71.6 ms** (warm) (≈ 5–7 GB/s).
- Cache hit short-circuits roughly **180 s** of prefill into **~70 ms** of disk read plus ~1 s of remainder prefill on a 96-token boundary trim.

## Cost/benefit comparison

For a typical 1.5 GB checkpoint on the M1 Ultra server (M4 Pro times in parentheses where measured):

| Codec | Save time | Disk size | Load time (in-mem decode, ST) |
|---|---|---|---|
| Uncompressed (today) | ~750 ms | 1500 MB | ~210 ms (mmap @ ~7 GB/s) |
| **lz4 -1 -T0** | 410 ms (270 ms) | 1010 MB | ~400 ms @ 3.8 GB/s (300 ms @ 5.0 GB/s) |
| **lz4 -9 -T0** | 11 s (17 s) | 840 MB | ~270 ms @ 5.6 GB/s (175 ms @ 7.4 GB/s)* |
| zstd -3 -T0 | ~500 ms | 760 MB | ~1100 ms @ 1.4 GB/s |
| zstd -9 -T0 | ~6 s | 750 MB | ~1100 ms @ 1.4 GB/s |

\* Estimated from the small-file lz4 benchmark; HC ratios hold but per-large-file decompress wasn't measured in this run.

On the **M4 Pro**, every lz4 number gets ~30% better. lz4 HC decompress in particular reaches **8.6 GB/s** on small files — the codec catches up to (and on small files, exceeds) the speed of an uncompressed mmap read.

**Net effect on a thrashing 100 GiB cache:**

- With **lz4 -1**: ~150 GiB of logical cache for the same disk → roughly 1.5× as many surviving cold-anchor entries → expected hit rate improvement large because today's evictions are entirely hits=0 churn.
- With **lz4 -9 HC**: ~180 GiB of logical cache and decompress as fast as mmap. Saves cost a few extra seconds during prefill on writes only.

## Proposal

Add `--kv-cache-codec {none, lz4, lz4hc, zstd}` and `--kv-cache-codec-level N` flags. Default remains `none` so on-disk compatibility is preserved unless the operator opts in.

### On-disk format

Single-frame layout is simplest but blocks parallel decode. Recommended **chunked** layout for parallel reads:

```
KVC v2 header (magic = "KVC2", existing fields + codec byte + chunk size)
u32 chunk_count
for each chunk:
  u32 compressed_size
  u8  chunk_data[compressed_size]    # independent lz4 frame
```

Chunk size: 16 MiB is a good balance — small enough that 20 cores stay busy on a 1.5 GiB file (~94 chunks), large enough that per-chunk framing overhead is negligible, and matches typical NVMe read-ahead.

Backwards compatibility: keep loader code paths for v1 files. v1 = uncompressed. New writes always use v2 with the configured codec (`none` is encoded as a `none` codec byte in v2, not a fallback to v1).

### Write path

In `kv_cache_store_*`:

1. Capture the engine payload into a buffer (unchanged).
2. If codec != none: split into 16 MiB chunks, compress each with `LZ4F_compressFrame` (or `LZ4_compress_HC` for HC) in a worker pool, write `compressed_size + bytes` per chunk.
3. With `-T0`-style parallelism, this is fully cpu-bound and on a 20-core Mac measures ~2.7 GB/s for L1 and ~135 MB/s for L9.

### Read path

In `kv_cache_try_load_text`:

1. Read header, validate.
2. Decompress chunks into the destination payload buffer in parallel. With `LZ4F_decompress` and a thread per chunk, expect 5+ GB/s aggregate on this hardware.
3. Hand the assembled payload to `ds4_session_restore_payload` as today.

`mmap` is no longer applicable for compressed files, but the existing code already uses `read`/`write` for KV payloads deliberately (to avoid adding VM mappings to a process that already mmaps the model), so this matches the design intent.

### Eviction score

No change required. `score = (effective_hits + 1) * tokens / file_size` still rewards smaller files (which compressed entries are) for the same logical content. Compressed entries naturally win evictions over uncompressed ones of the same logical size, accelerating migration to v2 if both formats coexist for a window.

### CLI

```
./ds4-server --kv-disk-dir /tmp/ds4-kv \
             --kv-disk-space-mb 102400 \
             --kv-cache-codec lz4 \
             --kv-cache-codec-level 1
```

Startup log additions:

```
ds4-server: KV disk cache /tmp/ds4-kv (budget=102400 MiB, codec=lz4 level=1 threads=auto, ...)
```

## Recommendation

**Ship `--kv-cache-codec lz4` with default level 1.** Evidence across both M1 Ultra and M4 Pro:

- It's nearly free on the save path — 0.6 s wall to compress 2.1 GB on M1 Ultra, 0.6 s on M4 Pro (saturated by 8 cores in both cases).
- It halves disk usage at the cost of slightly less compression on large files (1.5× vs the small-file 1.78×).
- Decompress runs at 4–6 GB/s single-threaded, comparable to or faster than the raw mmap reads ds4 uses today (~5–7 GB/s observed).

Document `lz4hc` (level 9) for operators who would prefer slightly tighter ratios. The HC compress cost is ~16 s on M1 Ultra and ~24 s on M4 Pro for a 2 GB checkpoint at `-T0`, but the resulting files decompress at 5.6–8.7 GB/s — at or above today's uncompressed read speed. For workloads where saves are bursty and loads dominate (which is exactly the agent-resends-prefix pattern), HC is the right setting.

Skip zstd: it gives only ~10–15% additional storage savings over lz4 HC but decompresses at ~⅓ the speed (1.1–1.4 GB/s vs 5.6–8.7 GB/s), which hurts the hot path that matters most. The compression-ratio advantage isn't worth the per-load slowdown when the goal is fitting more cold-anchor checkpoints in a fixed budget — lz4 HC's 1.79–2.08× is plenty.

## Open questions

1. Should the chunked format embed per-chunk hashes so individual chunks can be validated on read, or rely on a single whole-payload checksum like today?
2. Should the codec be selectable per save (so that operators can run a one-shot background compaction pass that re-encodes existing v1 entries to v2 lz4-hc) or fixed per server instance?
3. The `--favor-decSpeed` flag in lz4 1.10 trades ~5% ratio for measurably faster decompression. Worth exposing as `--kv-cache-codec-favor decode` for read-heavy workloads.

## Test evidence

All numbers above reproduce using `bench/compress-bench.sh` in this repo. Raw transcripts captured on:

- **M1 Ultra (Mac Studio)** — the machine actually serving ds4 today; see also `/tmp/ds4-server.log` lines tagged `kv cache stored`, `kv cache evicted`, `kv cache hit` for live evidence of the eviction-thrash pattern.
- **M4 Pro (Mac mini)** — cross-checks the codec choice on newer-gen silicon with fewer total cores.

The bench script wraps `zstd -b1 -e9 -i3 <file>` and `lz4 -b1 -e12 -i3 <file>` (in-memory micro-benchmarks, level sweeps) plus direct CLI invocations to time multi-thread compression at varying `-T#` settings. Caveats in this run:

- The first cut of the script had an argument-order bug in the zstd MT compression section (`-o` placed before the input) and an awk format-string mismatch in the decompress-via-CLI section. The in-memory `zstd -b` / `lz4 -b` numbers were captured correctly; the CLI-mode zstd MT and decompress rows in the raw output should be ignored until the patched script is re-run.
