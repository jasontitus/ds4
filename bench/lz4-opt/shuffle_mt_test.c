/* Multi-threaded HC+byte-4-shuffle benchmark for the sidecar use case.
 *
 * Matches ds4's parallel codec shape: 16 MiB chunks, N worker threads
 * (default 8) each handling its share of chunks.  Always byte-4 NEON
 * shuffle (it's a strict win on real KV data — see hc_shuffle_test
 * results).
 *
 * Levels tested: HC3 (LZ4HC min), HC6, HC9 (LZ4HC default), HC12 (max).
 *
 * Usage:  shuffle_mt_test RAW.raw [threads=8]
 */
#include "lz4.h"
#include "lz4hc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
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

#define CHUNK_BYTES (16u << 20)

static double now_sec(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1.0e6;
}

#if HAS_NEON
static void shuffle_byte4_neon(const uint8_t *in, uint8_t *out, size_t n) {
    size_t m = n / 4;
    size_t i = 0;
    for (; i + 64 <= m * 4; i += 64) {
        uint8x16x4_t v = vld4q_u8(in + i);
        size_t pos = i / 4;
        vst1q_u8(out + 0 * m + pos, v.val[0]);
        vst1q_u8(out + 1 * m + pos, v.val[1]);
        vst1q_u8(out + 2 * m + pos, v.val[2]);
        vst1q_u8(out + 3 * m + pos, v.val[3]);
    }
    for (; i < m * 4; i++) {
        int b = i & 3;
        out[b * m + (i >> 2)] = in[i];
    }
    size_t tail = n - m * 4;
    if (tail) memcpy(out + m * 4, in + m * 4, tail);
}
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

typedef struct {
    const uint8_t *raw_in;
    uint8_t *raw_out;
    int my_start, my_end;
    uint8_t **shuf;
    uint8_t **comp;
    int *comp_size;
    int comp_cap;
    int level;          /* 1 = fast (LZ4_compress_default); >=3 = HC level */
    int ok;
} work_t;

static void *compress_worker(void *p) {
    work_t *w = p;
    w->ok = 1;
    for (int c = w->my_start; c < w->my_end; c++) {
        const uint8_t *src = w->raw_in + (size_t)c * CHUNK_BYTES;
#if HAS_NEON
        shuffle_byte4_neon(src, w->shuf[c], CHUNK_BYTES);
#endif
        int n;
        int real_level = w->level & 0xFF;
        int use_hc = (w->level & 0x100) || (real_level >= 3);
        if (!use_hc) {
            n = LZ4_compress_default((const char *)w->shuf[c], (char *)w->comp[c],
                                     CHUNK_BYTES, w->comp_cap);
        } else {
            n = LZ4_compress_HC((const char *)w->shuf[c], (char *)w->comp[c],
                                CHUNK_BYTES, w->comp_cap, real_level);
        }
        if (n <= 0) { w->ok = 0; return NULL; }
        w->comp_size[c] = n;
    }
    return NULL;
}

static void *decompress_worker(void *p) {
    work_t *w = p;
    w->ok = 1;
    for (int c = w->my_start; c < w->my_end; c++) {
        uint8_t *dst = w->raw_out + (size_t)c * CHUNK_BYTES;
        int n = LZ4_decompress_safe((const char *)w->comp[c], (char *)w->shuf[c],
                                    w->comp_size[c], CHUNK_BYTES);
        if (n != (int)CHUNK_BYTES) { w->ok = 0; return NULL; }
#if HAS_NEON
        unshuffle_byte4_neon(w->shuf[c], dst, CHUNK_BYTES);
#endif
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s RAW.raw [threads=8]\n", argv[0]); return 2; }
    int n_threads = argc > 2 ? atoi(argv[2]) : 8;

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }
    struct stat st; fstat(fileno(fp), &st);
    size_t sz = (size_t)st.st_size;
    int n_chunks = (int)(sz / CHUNK_BYTES);
    if (n_chunks == 0) { fprintf(stderr, "< 1 chunk\n"); return 1; }
    size_t bench_sz = (size_t)n_chunks * CHUNK_BYTES;

    uint8_t *raw = malloc(bench_sz);
    if (fread(raw, 1, bench_sz, fp) != bench_sz) { perror("fread"); return 1; }
    fclose(fp);
    uint8_t *raw_out = malloc(bench_sz);

    int comp_cap = LZ4_compressBound(CHUNK_BYTES) + 64;
    uint8_t **shuf = calloc(n_chunks, sizeof(*shuf));
    uint8_t **comp = calloc(n_chunks, sizeof(*comp));
    int *comp_size = calloc(n_chunks, sizeof(*comp_size));
    for (int c = 0; c < n_chunks; c++) {
        shuf[c] = malloc(CHUNK_BYTES);
        comp[c] = malloc(comp_cap);
    }

    work_t W[64];
    pthread_t T[64];
    printf("# %s — %zu raw bytes, %d × 16 MiB chunks, %d threads, NEON byte-4 shuffle on every variant\n",
           argv[1], bench_sz, n_chunks, n_threads);
    printf("# %-8s %12s %8s %12s %12s\n", "level", "comp_size", "ratio", "C agg MB/s", "D agg MB/s");

    struct { const char *label; int level; } variants[] = {
        {"lz4-1", 1},
        {"HC1",   1 | 0x100},  /* tag HC1/HC2 so compress_worker knows to use HC, not fast */
        {"HC2",   2 | 0x100},
        {"HC3",   3},
        {"HC6",   6},
        {"HC9",   9},
    };
    for (size_t v = 0; v < sizeof(variants)/sizeof(variants[0]); v++) {
        int per = (n_chunks + n_threads - 1) / n_threads;
        for (int t = 0; t < n_threads; t++) {
            int s = t * per;
            int e = (s + per) < n_chunks ? (s + per) : n_chunks;
            W[t] = (work_t){
                .raw_in = raw, .raw_out = raw_out,
                .my_start = s, .my_end = e,
                .shuf = shuf, .comp = comp, .comp_size = comp_size,
                .comp_cap = comp_cap, .level = variants[v].level,
            };
        }
        double t0 = now_sec();
        for (int t = 0; t < n_threads; t++) pthread_create(&T[t], NULL, compress_worker, &W[t]);
        for (int t = 0; t < n_threads; t++) pthread_join(T[t], NULL);
        double dt_c = now_sec() - t0;

        uint64_t total_comp = 0;
        for (int c = 0; c < n_chunks; c++) total_comp += comp_size[c];

        memset(raw_out, 0, bench_sz);
        t0 = now_sec();
        for (int t = 0; t < n_threads; t++) pthread_create(&T[t], NULL, decompress_worker, &W[t]);
        for (int t = 0; t < n_threads; t++) pthread_join(T[t], NULL);
        double dt_d = now_sec() - t0;

        if (memcmp(raw, raw_out, bench_sz) != 0) {
            fprintf(stderr, "ROUND-TRIP MISMATCH level=%s\n", variants[v].label);
            return 1;
        }
        printf("  %-8s %12llu %7.3fx %12.1f %12.1f\n",
               variants[v].label, (unsigned long long)total_comp,
               (double)bench_sz / (double)total_comp,
               (bench_sz / 1.0e6) / dt_c,
               (bench_sz / 1.0e6) / dt_d);
    }
    return 0;
}
