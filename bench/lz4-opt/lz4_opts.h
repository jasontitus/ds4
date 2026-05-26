/* Central optimization flag definitions for the LZ4-opt benchmarking
 * framework. Every experimental optimization is gated behind one of these
 * macros so we can enable/disable them independently and study pairwise
 * interactions.
 *
 * Build variants pass -DLZ4_OPT_FOO=1 via OPT_CFLAGS. Defaults are 0.
 *
 * When adding a new optimization:
 *   1. Add an LZ4_OPT_<NAME> default-0 below.
 *   2. Append it to LZ4_OPT_BANNER_FMT / LZ4_OPT_BANNER_ARGS.
 *   3. Add the same name to OPTS in scripts/run_grid.py.
 *   4. Implement the change in lz4/lib/lz4*.c guarded by `#if LZ4_OPT_<NAME>`.
 *      Add `#include "lz4_opts.h"` near the top of that file the first time.
 */
#ifndef LZ4_OPTS_H
#define LZ4_OPTS_H

/* ===== Compression-side experiments ===== */

/* Branchless safe-address (cmov-style) replacement for the
 * `matchIndex + LZ4_DISTANCE_MAX < current` boundary check in
 * LZ4_compress_generic_validated. Inspired by zstd PR #4165
 * (+30% on M1 Pro for 32 KB blocks). */
#ifndef LZ4_OPT_FAST_CMOV
#define LZ4_OPT_FAST_CMOV 0
#endif

/* Prefetch upcoming hash table entries during fast-mode compression. */
#ifndef LZ4_OPT_FAST_PREFETCH_HT
#define LZ4_OPT_FAST_PREFETCH_HT 0
#endif

/* Page-align the LZ4 hash table (16 KB pages on Apple Silicon) to avoid
 * the ~28-cycle page-cross penalty on hash probes. */
#ifndef LZ4_OPT_HT_PAGE_ALIGN
#define LZ4_OPT_HT_PAGE_ALIGN 0
#endif

/* ===== LZ4-HC experiments ===== */

/* Prefetch ahead in the chainTable during LZ4HC_InsertAndGetWiderMatch. */
#ifndef LZ4_OPT_HC_PREFETCH
#define LZ4_OPT_HC_PREFETCH 0
#endif

/* Walk the HC hash chain as multiple interleaved sub-chains to expose more
 * ILP to the OoO window. Inspired by dougallj's M1 CRC32 work
 * (12 parallel chains for ~3x throughput). */
#ifndef LZ4_OPT_HC_INTERLEAVE
#define LZ4_OPT_HC_INTERLEAVE 0
#endif

/* ===== Decompression-side experiments ===== */

/* Use NEON TBL (vqtbl1q_u8) to replicate small-offset patterns instead of
 * byte-by-byte fallback in LZ4_memcpy_using_offset. */
#ifndef LZ4_OPT_DEC_TBL_REPLICATE
#define LZ4_OPT_DEC_TBL_REPLICATE 0
#endif

/* Vectorize read_variable_length() — replace serial 0xFF loop with NEON
 * vceqq_u8 + minv early-exit. */
#ifndef LZ4_OPT_DEC_VARLEN_NEON
#define LZ4_OPT_DEC_VARLEN_NEON 0
#endif

/* Force single-quadword loads instead of LDP in the fast decode loop's
 * inner copy, to preserve store-to-load forwarding on Apple Silicon
 * (LDP/STP forwarding does not fire on M-series). */
#ifndef LZ4_OPT_DEC_NO_LDP
#define LZ4_OPT_DEC_NO_LDP 0
#endif

/* ===== Helper macros ===== */

#if defined(__GNUC__) || defined(__clang__)
#define LZ4_OPT_PREFETCH(addr)   __builtin_prefetch((const void *)(addr), 0, 1)
#define LZ4_OPT_PREFETCH_W(addr) __builtin_prefetch((void *)(addr), 1, 1)
#define LZ4_OPT_LIKELY(x)        __builtin_expect(!!(x), 1)
#define LZ4_OPT_UNLIKELY(x)      __builtin_expect(!!(x), 0)
#else
#define LZ4_OPT_PREFETCH(addr)   ((void)0)
#define LZ4_OPT_PREFETCH_W(addr) ((void)0)
#define LZ4_OPT_LIKELY(x)        (x)
#define LZ4_OPT_UNLIKELY(x)      (x)
#endif

#if defined(__aarch64__) || defined(__arm64__)
#define LZ4_OPT_AARCH64 1
#else
#define LZ4_OPT_AARCH64 0
#endif

#if LZ4_OPT_AARCH64 && \
    (LZ4_OPT_DEC_TBL_REPLICATE || LZ4_OPT_DEC_VARLEN_NEON || LZ4_OPT_DEC_NO_LDP)
#include <arm_neon.h>
#endif

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#define LZ4_OPT_APPLE_SILICON 1
#else
#define LZ4_OPT_APPLE_SILICON 0
#endif

/* Single-line banner emitted by bench/verify so each result is
 * unambiguously tied to the flag set it was built with. */
#define LZ4_OPT_BANNER_FMT \
    "fast_cmov=%d ht_page_align=%d fast_pref_ht=%d " \
    "hc_pref=%d hc_inter=%d " \
    "dec_tbl=%d dec_varlen_neon=%d dec_no_ldp=%d"

#define LZ4_OPT_BANNER_ARGS \
    LZ4_OPT_FAST_CMOV, LZ4_OPT_HT_PAGE_ALIGN, LZ4_OPT_FAST_PREFETCH_HT, \
    LZ4_OPT_HC_PREFETCH, LZ4_OPT_HC_INTERLEAVE, \
    LZ4_OPT_DEC_TBL_REPLICATE, LZ4_OPT_DEC_VARLEN_NEON, LZ4_OPT_DEC_NO_LDP

#endif /* LZ4_OPTS_H */
