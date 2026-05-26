# LZ4 optimization options — status, results, and future work

This doc captures every `LZ4_OPT_*` experiment wired into the framework,
what it does, where it lives in the LZ4 source, what we've observed on
Apple Silicon so far, and what's worth investigating next.

Use this as the **pick-up-the-work** reference. The framework lives in
`bench/lz4-opt/`; opts are toggled by passing `-DLZ4_OPT_FOO=1` via
`OPT_CFLAGS`, or by listing them on the `scripts/run_grid.py --custom` line.

Each opt below has:
- **What it does**: the theory of why this could help.
- **Where it lives**: file:line of the change.
- **Source**: published research/PR that inspired it.
- **M-series result (so far)**: from the first singles run on the laptop.
- **What to investigate**.

---

## How to interpret "results so far"

The single grid run we have was on `Jason-MacBook` (M-class). Numbers are
%-change vs `baseline` in compress (C%) and decompress (D%) MB/s, median
of 11 iterations after 3 warmup runs. **Caveats:**

- Two opts had implementation bugs that produced extreme but meaningless
  numbers (`fast_prefetch_ht` -87% C, `hc_interleave` -62% C on HC9). Both
  are fixed in the current tree; rerun for real numbers.
- Several fast-path benches saturated clock_gettime ticks (visible as
  multiple variants reporting identical 22 GB/s on `random` / `q4` / `q8`).
  Fixed by scaling those corpora to 16 MB and bumping default iters to 25.
- Background processes and thermal pressure on a laptop introduce ±3–5%
  noise. Treat anything in that band as zero signal.

After the next grid run we'll have clean numbers. The "M-series result"
notes below are from the **buggy run** and should be replaced; for now
they indicate *direction* but not magnitude.

---

## Summary table — pick what to dig into

| Flag                          | Side    | Status            | First-run signal       | Recommendation                |
|-------------------------------|---------|-------------------|------------------------|-------------------------------|
| `LZ4_OPT_FAST_CMOV`           | compress| working           | ~neutral (±3%)         | low priority — codegen study  |
| `LZ4_OPT_FAST_PREFETCH_HT`    | compress| **just fixed**    | rerun for clean number | high priority — main hot loop |
| `LZ4_OPT_HT_PAGE_ALIGN`       | compress| working           | neutral, slight regress| reconsider implementation     |
| `LZ4_OPT_HC_PREFETCH`         | LZ4-HC  | working           | ~neutral on HC9        | needs deeper-corpus test      |
| `LZ4_OPT_HC_INTERLEAVE`       | LZ4-HC  | **just fixed**    | rerun for clean number | needs careful pair-test       |
| `LZ4_OPT_DEC_TBL_REPLICATE`   | decode  | working           | mostly neutral         | needs RLE-heavier corpus      |
| `LZ4_OPT_DEC_VARLEN_NEON`     | decode  | working           | **+10% on q4/q8/random**| highest-value to validate    |
| `LZ4_OPT_DEC_NO_LDP`          | decode  | working           | ~neutral               | reconsider; may need asm peek |

---

## 1. `LZ4_OPT_FAST_CMOV` — branchless distance check

**What it does.** The fast-mode match finder in
`LZ4_compress_generic_validated` has an unpredictable branch:
```c
if ( (matchIndex + LZ4_DISTANCE_MAX < current) ) continue;
```
The "is this candidate too far?" check mispredicts on workloads with
high hash-collision rates. This opt rewrites the block so that:
1. The load `LZ4_read32(match)` is rerouted through a known-safe pointer
   (`ip` itself, which is always in valid memory) when the candidate
   would have been out of range.
2. All three failure conditions (`too_far`, `dictSmallOOR`, `m32 != ip32`)
   are bitwise-OR'd into a single predicate.

The intent: replace the multi-branch decision tree with one CSEL + one
predictable branch. zstd PR #4165 measured +30% on M1 Pro using this same
trick.

**Where it lives.** `lz4/lib/lz4.c`, inside the `byU32`/`byU16` arm of the
match-finder loop, lines ~1098-1115. Guarded by `#if LZ4_OPT_FAST_CMOV`.

**Source.**
- [zstd PR #4165](https://github.com/facebook/zstd/pull/4165) — accepted version
- [zstd PR #4144](https://github.com/facebook/zstd/pull/4144) — earlier attempt with M1 Pro benches showing +30%

**M-series result (so far).** Within ±3% on every bench. The expected win
didn't materialize. **Likely explanation:** Apple clang on `-O3 -mcpu=native`
is already generating CSEL for the original short-circuit pattern, or the
M-series branch predictor handles the original well enough that removing
the branch doesn't pay. The zstd team had to fall back to inline asm because
"portable C code often failed to generate efficient conditional move
instructions across different compiler versions" — same issue.

**What to investigate.**
- Inspect codegen: `make VARIANT=cmov OPT_CFLAGS='-DLZ4_OPT_FAST_CMOV=1 -S -fverbose-asm'`
  → check that the inner loop actually uses CSEL.
- If not, try an inline-asm variant for arm64 only.
- Try the inverse: enable `__builtin_expect(too_far, 0)` on the original
  short-circuit branch — sometimes telling the compiler the branch is
  predictable is enough.
- Test on `json` and `kvcache` workloads at level 0 with 4 KB blocks
  where the match finder thrashes most.

---

## 2. `LZ4_OPT_FAST_PREFETCH_HT` — prefetch next hash bucket

**What it does.** In the fast-mode probe loop, the inner load
`LZ4_getIndexOnHash(h, cctx->hashTable, tableType)` is a pointer
chase into a 32–64 KB hash table. The next iteration's hash is computed
one step ahead (`forwardH`); this opt issues a prefetch hint to warm the
cache line it will touch.

**Where it lives.** `lz4/lib/lz4.c`, immediately after the
`LZ4_putIndexOnHash` write in the byU32/byU16 branch, ~lines 1088-1100.
Guarded by `#if LZ4_OPT_FAST_PREFETCH_HT`.

**Source.** zstd v1.5.4 "prefetching CDict tables" (+10-20% cold dict
compression). General-principle Apple-Silicon: the M-series load queue
holds ~130 outstanding loads, so explicit prefetching pays.

**M-series result (so far).** **DO NOT TRUST** the prior run — the old
code multiplied the hash by 4 bytes per entry unconditionally, but the
byU16 table uses 2-byte entries. For byU16 we were prefetching addresses
*past* the 64 KB hash table, hitting cold/unmapped memory and burning
TLB walks. That's why compress collapsed from 656→182 MB/s on `ascii-1M-blk4k`.

**Now fixed** to compute `forwardH * stride` where stride is the actual
per-entry width per tableType. Both `tableType` and `LZ4_HASHLOG` are
compile-time constants in the `FORCE_INLINE`'d template, so the branch
folds away. **Rerun singles for the real number.**

**What to investigate.**
- Whether this is actually fetched ahead or whether the prefetch lands
  too late (the hot loop is small, ~30-50 ns/iteration on M-series).
- A 2-deep variant: also prefetch one further ahead, computed from
  `forwardIp + step`.
- Whether the same prefetch helps in `LZ4_compress_HC` (different code).

---

## 3. `LZ4_OPT_HT_PAGE_ALIGN` — 16 KB-aligned hash table

**What it does.** Apple Silicon uses 16 KB virtual memory pages. If the
LZ4 hash table (64 KB, 4 pages on M-series) straddles a page boundary
the wrong way, every hash probe near that boundary eats a 28-cycle
page-cross penalty. This opt switches `LZ4_compress_fast` to use
`posix_memalign(16384, ...)` for the stream state, guaranteeing the
hash table is page-aligned.

**Where it lives.** `lz4/lib/lz4.c`, replacement allocation block in
`LZ4_compress_fast` (~lines 1483-1505). Guarded by `#if LZ4_OPT_HT_PAGE_ALIGN`.

**Source.** ocxtal/dougallj M-series microbench notes: "unaligned loads
put a penalty of one clock cycle. The page-crossing penalty is around
28 cycles."

**M-series result (so far).** Mostly neutral (-3% to +6%). The cost of
the per-call `posix_memalign` + `free` appears to roughly cancel any
page-cross win. The stack-allocated baseline is on a 16-byte boundary,
not 16 KB, but the actual layout under Apple's stack pointer typically
keeps the 64 KB table within 1-2 pages anyway.

**What to investigate.**
- Allocate the stream **once** at the bench level and reuse it across
  all blocks (avoid per-call malloc). The current implementation
  re-allocates on every `LZ4_compress_default` call, which is the
  benchmarking pattern but not the real-world one for streaming.
  → If we change the bench harness to keep a reusable stream, this
  opt's true effect becomes visible.
- An alternative: align via `alignas(16384)` on a static buffer.
- Measure with `vmmap` / `pmap` on the bench process to confirm the
  hash table is actually crossing pages under the baseline.

---

## 4. `LZ4_OPT_HC_PREFETCH` — prefetch chain target in LZ4-HC

**What it does.** `LZ4HC_InsertAndGetWiderMatch` walks a hash chain
(linked list of prior positions with the same hash). Each step:
1. Read 2-byte delta from `chainTable`.
2. Subtract to get next `matchIndex`.
3. Compute `matchPtr = prefixPtr + (matchIndex - prefixIdx)`.
4. Read 4 bytes from `matchPtr` to compare with `pattern`.

Step 4 is a dependent load on (3), which is dependent on (1). This opt
issues a prefetch for `matchPtr` after each step's chain update, so the
next iteration's read finds the cache line warm.

**Where it lives.** `lz4/lib/lz4hc.c`, immediately after
`matchIndex -= DELTANEXTU16(...)` at ~line 1099. Guarded by
`#if LZ4_OPT_HC_PREFETCH`.

**Source.** Same as #2 — general Apple-Silicon ILP-exposure principle.

**M-series result (so far).** ~Neutral on `ascii-HC9` and `json-HC9`.
The chain depth on these corpora may be too short to benefit (only a
few iterations before the chain terminates), OR Apple's hardware
prefetcher is already detecting the pattern.

**What to investigate.**
- Test on `kvcache` HC9 — block-structured data with repeating headers
  should produce long chains with cold tail entries.
- Test at higher HC levels (12) where `nbAttempts` is much larger and
  prefetch latency-hiding wins compound.
- Profile with `perf` / `Instruments` to confirm chain-walk is actually
  L1/L2-bound vs. ALU-bound.

---

## 5. `LZ4_OPT_HC_INTERLEAVE` — deeper look-ahead in HC chain walk

**What it does.** Extends #4: in addition to prefetching the next
iteration's source data, speculatively follow one more chain link and
prefetch the iteration-after-next's source. Doubles the memory-level
parallelism the OoO window sees.

**Where it lives.** `lz4/lib/lz4hc.c`, same region as #4, guarded by
`#if LZ4_OPT_HC_INTERLEAVE`.

**Source.** dougallj's M1 CRC32 work: 12 parallel dependency chains
beat 4 parallel chains by 3×. The same principle applies to any
latency-bound dependency chain.

**M-series result (so far).** **DO NOT TRUST** the prior run —
the buggy version computed `peek = matchIndex - peek_delta` without
checking that `peek` was still in range. When `peek_delta` was a sentinel
(0xFFFF) we'd compute a wildly negative `peek`, then prefetch
`prefixPtr + (peek - prefixIdx)` which was a wild pointer hundreds of
MB away. That tanked HC9 by ~60% from TLB walks.

**Now fixed** to require both `peek >= lowestMatchIndex` and
`peek >= prefixIdx` before issuing the prefetch. **Rerun for real numbers.**

**What to investigate.**
- Pair with `LZ4_OPT_HC_PREFETCH`: do they compose or overlap?
- A 3-deep version (prefetch 3 ahead) if 2-deep clearly helps.
- Try the structurally-different approach: rather than look-ahead in
  one chain, process two *input positions* in parallel (their chains
  are independent).

---

## 6. `LZ4_OPT_DEC_TBL_REPLICATE` — NEON pattern replicator for small offsets

**What it does.** In the decompressor's fast loop, when a match's offset
is in [1..7], the byte-at-a-time fallback in `LZ4_memcpy_using_offset_base`
is slow on every iteration. This opt builds a 16-byte replicated pattern
in a single NEON `vqtbl1q_u8` shuffle (using a precomputed 8×16 index
table) and stamps it down with one `vst1q_u8`. The rest of the match
length is then a normal wildcopy from an offset of 16 (which is safe
for the unaligned 8-byte wildcopy loop).

**Where it lives.** `lz4/lib/lz4.c`, replacement fast-path in
`LZ4_memcpy_using_offset` at ~lines 530-575. Guarded by
`#if LZ4_OPT_AARCH64 && LZ4_OPT_DEC_TBL_REPLICATE`.

**Source.** simdjson Apple Silicon optimization thread (`vqtbl2q_u8`
for whitespace classification). General Apple-Silicon: TBL has 2-cycle
latency fully pipelined; an entire 1-byte replication finishes in two
instructions vs the 16-byte sequential write the scalar path emits.

**M-series result (so far).** ~Neutral on most benches. Probably because
the synthetic corpora don't have enough small-offset matches to make this
fast-path matter (it's a fraction of decoded matches).

**What to investigate.**
- Build an explicit "RLE-heavy" corpus: text with long repeated runs
  separated by short tokens. The `repetitive` corpus is closest but
  most of its runs degenerate into a single offset=1 match per region.
- Real ds4 cache files: if header padding or zero runs decompress
  through this path, the win might show up.
- Profile to confirm how often the small-offset path is taken vs the
  offset ≥ 16 fast path. If <5% of matches hit it, this opt is a fix
  for a non-bottleneck.

---

## 7. `LZ4_OPT_DEC_VARLEN_NEON` — vectorized 0xFF chain skip

**What it does.** The `read_variable_length` function decodes the
multi-byte extension for literal/match lengths ≥ 15. The serial loop
reads one byte at a time and bails when `byte != 0xFF`. For long
literals (LZ4's RUN_MASK == 15) and long matches (ML_MASK == 15) the
chain can be dozens of bytes. This opt loads 16 bytes at a time via
NEON, builds a "first non-FF" mask via `vceqq_u8(v, 0xFF)` + the
`vshrn_n_u16` nibble-mask trick, and finds the terminating byte with
a single `ctz`.

**Where it lives.** `lz4/lib/lz4.c`, inside `read_variable_length`'s
`do {} while(s == 255)` block, ~lines 2080-2115. Guarded by
`#if LZ4_OPT_AARCH64 && LZ4_OPT_DEC_VARLEN_NEON`.

**Source.** Same `vshrn_n_u16` nibble-mask trick zstd PR #3139 used for
its row-hash match finder (and simdjson uses extensively).

**M-series result (so far).** **+10% decompress on q4-2M-blk64k,
+10% on q8-2M-blk64k, +13% on random-1M-blk64k.** This is the most
consistent positive signal across the singles grid. The corpora that
trigger long matches benefit; corpora with short matches see noise.

**What to investigate.**
- Confirm with the larger corpora in the new defaults (16 MB random,
  q4, q8).
- Whether the win compounds with `LZ4_OPT_DEC_NO_LDP` (they touch
  different parts of the decompress fast loop).
- Whether the 16-byte NEON load can be extended to 32 bytes (two
  `vld1q_u8` + `vpaddq_u8`) for even longer runs.
- The exact distribution of variable-length values in real ds4 cache
  decompression — if FF chains are short (≤2 bytes), the SIMD path's
  setup overhead may dominate the wins. Measure with `perf` on a
  real workload.

---

## 8. `LZ4_OPT_DEC_NO_LDP` — single 128-bit copy to preserve store-forward

**What it does.** In the fast decode loop, the offset≥8 match copy is:
```c
LZ4_memcpy(op, match, 8);
LZ4_memcpy(op+8, match+8, 8);
LZ4_memcpy(op+16, match+16, 2);
```
Clang typically fuses the first two into `ldp q0, q1, [match]; stp q0, q1, [op]`.
On Apple Silicon, store-to-load forwarding **does not fire** through pair
instructions (Lemire 2024) — so subsequent loads from the just-written
region eat L1 latency (3–5 cycles) instead of 1-cycle forwarding.

This opt replaces the two 8-byte memcpys with one explicit
`vst1q_u8(op, vld1q_u8(match))`, which the compiler emits as
single-quadword `ldr q0, [match]; str q0, [op]` — forwarding-eligible.

**Where it lives.** `lz4/lib/lz4.c`, fastpath match copy in the
FAST_DEC_LOOP at ~lines 2156-2169. Guarded by
`#if LZ4_OPT_AARCH64 && LZ4_OPT_DEC_NO_LDP`.

**Source.** [Lemire, "Careful with Pair-of-Registers instructions on Apple
Silicon"](https://lemire.me/blog/2024/04/29/careful-with-pair-of-registers-instructions-on-apple-silicon/).

**M-series result (so far).** ~Neutral (-3% to +1%). The theoretical win
relies on the next iteration's input read overlapping the just-written
output region — that only happens when matches are tightly packed. On the
test corpora, most iterations have enough other work between writes and
reads that the forwarding doesn't actually fire either way.

**What to investigate.**
- Disassemble the baseline to confirm clang IS emitting `ldp/stp` (it
  may not always — depends on inliner heuristics).
- Build a synthetic corpus with very short matches and dense back-references
  (e.g., 8-byte matches separated by 1-byte literals). That should
  maximize the store→load overlap.
- Apply the same treatment to `LZ4_wildCopy32` (line 524) which has the
  same `memcpy(d,s,16); memcpy(d+16,s+16,16);` pattern.
- Pair with `LZ4_OPT_DEC_TBL_REPLICATE` — they're in adjacent code regions.

---

## Cross-cutting future investigations

These don't map to a single existing opt; they're things that would
require new entries in the framework.

### A. Hashtable hot-set characterization
Measure the actual working set of the hash table during compression
of typical workloads. If only ~25% of the table is touched per block,
shrinking `LZ4_HASHLOG` to 12 (16 KB table) would fit in L1D entirely
and might beat the +prefetch experiments.

### B. Single-shared LZ4 stream
The current bench harness re-creates the LZ4 state per-block via
`LZ4_compress_default`. Real-world streaming APIs reuse it. Add a
bench mode that uses `LZ4_compress_fast_continue` with one persistent
stream. This is where `HT_PAGE_ALIGN` would actually show its expected
win.

### C. Hardware prefetcher friendliness
M-series prefetcher recognizes strided access. Hash table probes are
*pseudo-random*, so it can't help. Could we restructure the probe loop
to fetch a small batch of *consecutive* slots (linear probing) when a
miss is detected, letting the HW prefetcher kick in? Big algorithmic
change, but would map onto the M-series prefetcher's strengths.

### D. Decoder loop unrolling
The FAST_DEC_LOOP body is ~50 cycles of dependent work. On a 7-wide
decode core like M-series, unrolling by 2 (process two LZ4 sequences
back-to-back) might let the OoO window run them partly in parallel.
Big restructuring; high effort.

### E. Bench harness: pin to P-core on macOS
The bench currently runs at default QoS. Setting
`pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)` keeps
the bench process on a P-core consistently. Add to `bench.c` for the
`__APPLE__` case to reduce variance below ±2%.

### F. Direct comparison vs upstream LZ4 release tag
We're on `dev`. Cross-check `LZ4_compress_default` throughput vs the
`v1.10.0` release tag to make sure dev hasn't regressed in some way
we'd be inadvertently masking.

### G. Real ds4 cache file benchmarks
Synthetic `kvcache` is approximate. Run against actual on-disk DS4
KV cache files via `--corpus <path>`. The match-length distribution
of real cache data is the actual workload we care about — synthetic
results that look promising might not transfer.

---

## Reproducing this work on the laptop

```bash
cd ds4
git pull
cd bench/lz4-opt
make clean && make
./build/baseline/verify                              # → "780 cases run, 0 failures"

# Singles grid with the fixed opts and larger corpora.
python3 scripts/run_grid.py --mode singles           # default --iters 25 --warmup 5

# Pairs grid (~30 min, more if iters bumped further).
python3 scripts/run_grid.py --mode pairs

# A custom comparison — e.g., the two opts that showed signal:
python3 scripts/run_grid.py --mode custom \
    --custom 'LZ4_OPT_DEC_VARLEN_NEON' \
             'LZ4_OPT_DEC_NO_LDP' \
             'LZ4_OPT_DEC_VARLEN_NEON,LZ4_OPT_DEC_NO_LDP'

# Real-data bench (replace path):
./build/dec_varlen_neon/bench --corpus ~/ds4-cache/file.bin --block 65536 --iters 25
```

Each run writes `results/grid-<timestamp>.{md,jsonl}`.
The `.md` is the human-readable grid; the `.jsonl` log has every
iteration's raw timings if you want to recompute different statistics.

---

## How to add a new optimization (after the first round)

1. **Pick the hot-spot.** Use `Instruments` (Time Profiler) on
   `./build/baseline/bench --corpus <file>` to confirm where time goes.
2. **Add the flag.** `lz4_opts.h` gets a new `#ifndef LZ4_OPT_<NAME>` block
   defaulting to 0, plus an addition to `LZ4_OPT_BANNER_FMT/_ARGS`.
3. **Register with the grid runner.** Append `"LZ4_OPT_<NAME>"` to the
   `OPTS` list in `scripts/run_grid.py`.
4. **Implement the change.** Add `#if LZ4_OPT_<NAME> ... #else ... #endif`
   blocks in `lz4/lib/lz4.c` or `lz4hc.c`. If a NEON header is needed,
   `lz4_opts.h` already pulls `arm_neon.h` automatically when *any*
   NEON-using opt is on.
5. **Verify.** `make VARIANT=foo OPT_CFLAGS='-DLZ4_OPT_<NAME>=1' && ./build/foo/verify`
   must end with "780 cases run, 0 failures" before benching.
6. **Bench in isolation.**
   `python3 scripts/run_grid.py --mode custom --custom 'LZ4_OPT_<NAME>'`.
7. **Document.** Add an entry below the existing ones in this doc and
   record the result.

---

## Open questions worth thinking about

- **Block size sweet spot:** every modern compression workload defaults
  to ~64 KB blocks for a reason (cache fit). Should the bench include
  16 KB and 256 KB blocks to triangulate?
- **Compression level vs throughput Pareto:** we test fast-mode and HC9;
  the interesting space is HC1–HC4 where many production users actually
  live. Worth adding HC3 to default benches.
- **Frame format overhead:** all current tests use raw block API. LZ4
  frames add CRC + magic + per-block headers — that's another bench
  dimension and might reveal opts that matter for streamed-file use
  cases (and not for in-memory block compression).
- **AMX or SVE-on-future-Mx:** Apple hasn't shipped SVE yet, but if M5
  or M6 does, several of these opts become irrelevant and SVE2-shaped
  rewrites become the new frontier.
