/* sysbw - establish the system-level memory bandwidth ceiling for the
 * patterns LZ4 decompress hits. If our decompressed throughput is already
 * at or near these numbers, no amount of decode-loop optimization helps -
 * we'd be hitting the memory subsystem, not the decoder.
 *
 * The kernels are deliberately simple so they should run at the L1D/L2/SLC
 * limit (whichever applies for the buffer size). The progression:
 *
 *   memcpy             - libSystem's hand-tuned SIMD memcpy
 *   neon_copy_16       - vld1q_u8 + vst1q_u8 per 16 bytes
 *   neon_copy_32_ldp   - the LDP/STP pair pattern LZ4 wildCopy32 emits
 *   neon_copy_32_split - two single-quadword ldr/str pairs (our opt's pattern)
 *   neon_copy_64       - prefetched ldp/stp pairs
 *
 * Compare these MB/s against our LZ4 decompress numbers for the same
 * buffer size. If LZ4 is within ~30% of memcpy, the literal-copy hot
 * path is bandwidth-limited and decode-loop opts can't help much.
 */
#include "timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(__arm64__)
# include <arm_neon.h>
# define HAVE_NEON 1
#else
# define HAVE_NEON 0
#endif

static double mbps(size_t bytes, double ns) {
    if (ns <= 0) return 0;
    return ((double)bytes * 1000.0) / ns / 1.048576;
}

static void neon_copy_16(unsigned char *dst, const unsigned char *src, size_t n) {
#if HAVE_NEON
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        vst1q_u8(dst + i, vld1q_u8(src + i));
    }
    if (i < n) memcpy(dst + i, src + i, n - i);
#else
    memcpy(dst, src, n);
#endif
}

/* The 32-byte loop body Clang generates from LZ4_wildCopy32 baseline:
 * ldp q0,q1, [src]; stp q0,q1, [dst]. Single instruction issue per pair,
 * no store-forwarding through the pair. */
static void neon_copy_32_ldp(unsigned char *dst, const unsigned char *src, size_t n) {
#if HAVE_NEON
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint8x16_t a = vld1q_u8(src + i);
        uint8x16_t b = vld1q_u8(src + i + 16);
        vst1q_u8(dst + i,      a);
        vst1q_u8(dst + i + 16, b);
    }
    if (i < n) memcpy(dst + i, src + i, n - i);
#else
    memcpy(dst, src, n);
#endif
}

/* Our LZ4_OPT_DEC_WILDCOPY_NEON pattern: two single-quadword pairs forced
 * via inline asm so clang can't fuse into LDP/STP. */
static void neon_copy_32_split(unsigned char *dst, const unsigned char *src, size_t n) {
#if HAVE_NEON
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint8x16_t v0, v1;
        __asm__ __volatile__(
            "ldr  %q[v0], [%[s]]      \n\t"
            "str  %q[v0], [%[d]]      \n\t"
            "ldr  %q[v1], [%[s], #16] \n\t"
            "str  %q[v1], [%[d], #16] \n\t"
            : [v0] "=&w"(v0), [v1] "=&w"(v1)
            : [d]  "r"(dst + i), [s]  "r"(src + i)
            : "memory"
        );
    }
    if (i < n) memcpy(dst + i, src + i, n - i);
#else
    memcpy(dst, src, n);
#endif
}

static void neon_copy_64(unsigned char *dst, const unsigned char *src, size_t n) {
#if HAVE_NEON
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        uint8x16_t a = vld1q_u8(src + i);
        uint8x16_t b = vld1q_u8(src + i + 16);
        uint8x16_t c = vld1q_u8(src + i + 32);
        uint8x16_t d = vld1q_u8(src + i + 48);
        vst1q_u8(dst + i,      a);
        vst1q_u8(dst + i + 16, b);
        vst1q_u8(dst + i + 32, c);
        vst1q_u8(dst + i + 48, d);
    }
    if (i < n) memcpy(dst + i, src + i, n - i);
#else
    memcpy(dst, src, n);
#endif
}

typedef void (*kernel_fn)(unsigned char *, const unsigned char *, size_t);

static double bench_one(kernel_fn fn, unsigned char *dst, const unsigned char *src,
                        size_t n, int iters) {
    /* Warmup. */
    fn(dst, src, n);
    fn(dst, src, n);

    uint64_t t0 = bench_now_ns();
    for (int i = 0; i < iters; i++) fn(dst, src, n);
    uint64_t t1 = bench_now_ns();

    /* Best per-iteration. */
    return (double)(t1 - t0) / (double)iters;
}

int main(int argc, char **argv) {
    size_t sizes[] = {
        64 * 1024,                  /* L1-fitting */
        2 * 1024 * 1024,            /* L2-fitting */
        16 * 1024 * 1024,           /* SLC/L3 region */
        64 * 1024 * 1024,           /* Memory-bound */
        256 * 1024 * 1024,          /* Definitely memory */
    };
    int n_sizes = sizeof(sizes) / sizeof(sizes[0]);
    int iters = (argc > 1) ? atoi(argv[1]) : 30;

    size_t max_sz = 0;
    for (int i = 0; i < n_sizes; i++) if (sizes[i] > max_sz) max_sz = sizes[i];

    unsigned char *src = NULL, *dst = NULL;
    if (posix_memalign((void **)&src, 4096, max_sz) != 0 ||
        posix_memalign((void **)&dst, 4096, max_sz) != 0) {
        fprintf(stderr, "OOM\n"); return 1;
    }
    for (size_t i = 0; i < max_sz; i++) src[i] = (unsigned char)i;

    struct { const char *name; kernel_fn fn; } kernels[] = {
        { "memcpy",             (kernel_fn)memcpy },
        { "neon_copy_16",       neon_copy_16 },
        { "neon_copy_32_ldp",   neon_copy_32_ldp },
        { "neon_copy_32_split", neon_copy_32_split },
        { "neon_copy_64",       neon_copy_64 },
    };
    int n_kernels = sizeof(kernels) / sizeof(kernels[0]);

    printf("# system memory bandwidth ceiling (best-of-%d)\n", iters);
    printf("# size              kernel                MB/s\n");
    for (int s = 0; s < n_sizes; s++) {
        size_t sz = sizes[s];
        for (int k = 0; k < n_kernels; k++) {
            /* Repeat 3 times, take best (lowest ns), to suppress scheduler noise. */
            double best = 1e30;
            for (int rep = 0; rep < 3; rep++) {
                double ns = bench_one(kernels[k].fn, dst, src, sz, iters);
                if (ns < best) best = ns;
            }
            printf("  %10zu  %-22s  %10.1f\n", sz, kernels[k].name, mbps(sz, best));
        }
        printf("\n");
    }

    free(src); free(dst);
    return 0;
}
