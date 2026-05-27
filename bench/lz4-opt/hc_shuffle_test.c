/* Recompress real KV bytes at different LZ4 levels and with optional
 * bit-shuffling preprocess, to see whether either justifies a future
 * encoder change.
 *
 * Reads a .raw file (already-extracted raw KV bytes — use kv_extract.c
 * to produce them from a .kv file).  For each combination of:
 *   level   ∈ { lz4-1 (default), HC9, HC12 }
 *   shuffle ∈ { none, byte-2, byte-4, byte-8 }
 * measures:
 *   compressed size, ratio (raw/compressed),
 *   compress throughput (single-thread, MB/s of raw input),
 *   decompress throughput (single-thread, MB/s of raw output).
 *
 * Shuffle: standard "byte-of-element" transpose used by Blosc / HDF5.
 * For a buffer of N bytes interpreted as M elements of S bytes each:
 *   shuffled[b * M + i] = raw[i * S + b]   (b in [0,S), i in [0,M))
 * Tail bytes (n % S) pass through unmodified.  This collects bytes
 * from the same position across all elements together, which often
 * exposes more matches to LZ4 if the data has element-wise structure.
 * Unshuffle is the inverse pass.  Both passes are O(N), unvectorized
 * here for clarity.
 *
 * Usage:  hc_shuffle_test RAW.raw
 */
#include "lz4.h"
#include "lz4hc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__aarch64__) || defined(__arm64__)
#  include <arm_neon.h>
#  define HAS_NEON 1
#else
#  define HAS_NEON 0
#endif

static double now_sec(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1.0e6;
}

/* NEON byte-4 shuffle: vld4q_u8 de-interleaves 64 input bytes into four
 * 16-byte lanes (lane k = bytes at positions k, k+4, k+8, ...).  Lane k
 * is then written to out[k*m + ...], so the four position-streams end
 * up contiguous as required by the shuffle definition. */
#if HAS_NEON
static void shuffle_byte4_neon(const uint8_t *in, uint8_t *out, size_t n) {
    size_t m = n / 4;
    size_t i = 0;
    /* Each iteration consumes 64 input bytes and writes 16 to each
     * of the 4 output streams. */
    for (; i + 64 <= m * 4; i += 64) {
        uint8x16x4_t v = vld4q_u8(in + i);
        size_t pos = i / 4;
        vst1q_u8(out + 0 * m + pos, v.val[0]);
        vst1q_u8(out + 1 * m + pos, v.val[1]);
        vst1q_u8(out + 2 * m + pos, v.val[2]);
        vst1q_u8(out + 3 * m + pos, v.val[3]);
    }
    /* Scalar tail. */
    for (; i < m * 4; i++) {
        int b = i & 3;
        size_t e = i >> 2;
        out[b * m + e] = in[i];
    }
    size_t tail = n - m * 4;
    if (tail) memcpy(out + m * 4, in + m * 4, tail);
}

/* NEON byte-4 unshuffle: read 16 contiguous bytes from each of the four
 * input streams (out[k*m + ...]), vst4q_u8 interleaves them back into
 * 64 bytes of the original layout. */
static void unshuffle_byte4_neon(const uint8_t *in, uint8_t *out, size_t n) {
    size_t m = n / 4;
    size_t pos = 0;
    for (; pos + 16 <= m; pos += 16) {
        uint8x16x4_t v;
        v.val[0] = vld1q_u8(in + 0 * m + pos);
        v.val[1] = vld1q_u8(in + 1 * m + pos);
        v.val[2] = vld1q_u8(in + 2 * m + pos);
        v.val[3] = vld1q_u8(in + 3 * m + pos);
        vst4q_u8(out + pos * 4, v);
    }
    for (; pos < m; pos++) {
        for (int b = 0; b < 4; b++) out[pos * 4 + b] = in[b * m + pos];
    }
    size_t tail = n - m * 4;
    if (tail) memcpy(out + m * 4, in + m * 4, tail);
}
#endif

static void shuffle_bytes(const uint8_t *in, uint8_t *out, size_t n, int s) {
    if (s <= 1) { memcpy(out, in, n); return; }
#if HAS_NEON
    if (s == 4) { shuffle_byte4_neon(in, out, n); return; }
#endif
    size_t m = n / s;            /* number of full elements */
    size_t tail = n - m * s;
    for (int b = 0; b < s; b++) {
        for (size_t i = 0; i < m; i++) {
            out[b * m + i] = in[i * s + b];
        }
    }
    if (tail) memcpy(out + m * s, in + m * s, tail);
}

static void unshuffle_bytes(const uint8_t *in, uint8_t *out, size_t n, int s) {
    if (s <= 1) { memcpy(out, in, n); return; }
#if HAS_NEON
    if (s == 4) { unshuffle_byte4_neon(in, out, n); return; }
#endif
    size_t m = n / s;
    size_t tail = n - m * s;
    for (int b = 0; b < s; b++) {
        for (size_t i = 0; i < m; i++) {
            out[i * s + b] = in[b * m + i];
        }
    }
    if (tail) memcpy(out + m * s, in + m * s, tail);
}

typedef struct {
    const char *label;
    int level;          /* 1 = fast, 9 = HC9, 12 = HC12 */
} variant_t;

typedef struct {
    int shuffle;        /* 1 = none, 2/4/8 = byte stride */
} shuffle_t;

static int compress_one(const uint8_t *src, int src_sz,
                        uint8_t *dst, int dst_cap, int level) {
    if (level == 1) return LZ4_compress_default((const char *)src, (char *)dst, src_sz, dst_cap);
    return LZ4_compress_HC((const char *)src, (char *)dst, src_sz, dst_cap, level);
}

/* For our 16 MiB chunks we'd ideally bench each chunk separately;
 * but for headline-level shuffle/HC ratios on real data, doing the
 * whole file in one shot is what matters here (no per-chunk overhead
 * to obscure the ratio).  Compress speed is single-threaded so the
 * absolute throughput is for one core, comparable across variants. */
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s RAW.raw\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }
    struct stat st; fstat(fileno(fp), &st);
    size_t sz = (size_t)st.st_size;
    uint8_t *raw = malloc(sz);
    if (fread(raw, 1, sz, fp) != sz) { perror("fread"); return 1; }
    fclose(fp);

    /* Test at the production chunk size (16 MiB) so numbers match
     * the codec's actual working set.  Truncate to a multiple if
     * the file is bigger so we get a stable per-chunk measurement. */
    const size_t chunk = 16u << 20;
    size_t nchunks = sz / chunk;
    if (nchunks == 0) {
        fprintf(stderr, "file %s only %zu bytes, < one 16 MiB chunk\n", argv[1], sz);
        return 1;
    }
    size_t bench_sz = nchunks * chunk;
    printf("# %s — %zu bytes raw, benching %zu MiB (= %zu × 16 MiB chunks), single thread\n",
           argv[1], sz, bench_sz >> 20, nchunks);
    printf("# %-12s %-9s %12s %8s %10s %10s\n",
           "algo", "shuffle", "comp_size", "ratio", "C_MB/s", "D_MB/s");

    int comp_cap = LZ4_compressBound((int)chunk) + 64;
    uint8_t *shuf  = malloc(chunk);
    uint8_t *comp  = malloc(comp_cap);
    uint8_t *dec   = malloc(chunk);

    variant_t vars[] = {
        {"lz4-1", 1},
        {"HC9",   9},
        {"HC12", 12},
    };
    int shuffles[] = {1, 2, 4, 8};

    for (size_t v = 0; v < sizeof(vars)/sizeof(vars[0]); v++) {
        for (size_t s = 0; s < sizeof(shuffles)/sizeof(shuffles[0]); s++) {
            int shuf_n = shuffles[s];
            const char *sh_label = shuf_n == 1 ? "none" :
                                   shuf_n == 2 ? "byte-2" :
                                   shuf_n == 4 ? "byte-4" : "byte-8";

            /* Compress all chunks, accumulating total compressed size
             * and total compress time. */
            uint64_t total_comp = 0;
            double t_c = 0, t_d = 0;
            int round_trip_ok = 1;
            for (size_t c = 0; c < nchunks; c++) {
                const uint8_t *src = raw + c * chunk;
                const uint8_t *cs;

                /* Shuffle (or pass-through). */
                if (shuf_n > 1) {
                    shuffle_bytes(src, shuf, chunk, shuf_n);
                    cs = shuf;
                } else {
                    cs = src;
                }

                /* Compress. */
                double t0 = now_sec();
                int n = compress_one(cs, (int)chunk, comp, comp_cap, vars[v].level);
                t_c += now_sec() - t0;
                if (n <= 0) { fprintf(stderr, "compress fail\n"); return 1; }
                total_comp += (uint64_t)n;

                /* Decompress + unshuffle. */
                double t1 = now_sec();
                int rn = LZ4_decompress_safe((const char *)comp, (char *)dec,
                                             n, (int)chunk);
                if (rn != (int)chunk) { fprintf(stderr, "dec fail\n"); return 1; }
                if (shuf_n > 1) {
                    uint8_t *tmp = malloc(chunk);
                    unshuffle_bytes(dec, tmp, chunk, shuf_n);
                    memcpy(dec, tmp, chunk);
                    free(tmp);
                }
                t_d += now_sec() - t1;
                if (memcmp(dec, src, chunk) != 0) {
                    round_trip_ok = 0;
                    fprintf(stderr, "ROUND-TRIP MISMATCH chunk=%zu algo=%s shuf=%d\n",
                            c, vars[v].label, shuf_n);
                    break;
                }
            }
            if (!round_trip_ok) continue;

            double ratio = (double)bench_sz / (double)total_comp;
            double c_mbs = (bench_sz / 1.0e6) / t_c;
            double d_mbs = (bench_sz / 1.0e6) / t_d;
            printf("  %-12s %-9s %12llu %7.3fx %10.1f %10.1f\n",
                   vars[v].label, sh_label,
                   (unsigned long long)total_comp, ratio, c_mbs, d_mbs);
        }
    }
    free(raw); free(shuf); free(comp); free(dec);
    return 0;
}
