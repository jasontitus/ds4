/* lz4-opt bench
 *
 * Times LZ4 (or LZ4_HC at a given level) compress + decompress on a corpus,
 * computes throughput statistics across N iterations, and emits either a
 * human-readable summary or one JSON line for the grid runner to slurp.
 */
#include "lz4_opts.h"
#include "lz4/lib/lz4.h"
#include "lz4/lib/lz4hc.h"
#include "corpus.h"
#include "timing.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_MAX_ITERS 64
#define BENCH_MAX_THREADS 64

typedef struct {
    double v[BENCH_MAX_ITERS];
    int n;
} samples_t;

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void summarize(const samples_t *s,
                      double *med, double *minv, double *maxv, double *sdev) {
    double arr[BENCH_MAX_ITERS];
    memcpy(arr, s->v, s->n * sizeof(double));
    qsort(arr, s->n, sizeof(double), cmp_d);
    *minv = arr[0];
    *maxv = arr[s->n - 1];
    *med  = (s->n & 1) ? arr[s->n/2] : 0.5 * (arr[s->n/2 - 1] + arr[s->n/2]);
    double m = 0; for (int i = 0; i < s->n; i++) m += arr[i]; m /= s->n;
    double var = 0;
    for (int i = 0; i < s->n; i++) { double d = arr[i] - m; var += d*d; }
    *sdev = sqrt(var / s->n);
}

static double mbps(size_t bytes, double ns) {
    if (ns <= 0) return 0;
    return ((double)bytes * 1000.0) / ns / 1.048576; /* bytes/ns -> MB/s (MiB) */
}

/* Compress one corpus into a stream of (i32 length-prefix, payload) blocks.
 * Returns nanoseconds elapsed, or -1 on failure. */
static double do_compress(const corpus_file_t *cf, size_t block, int level,
                          unsigned char *cbuf, size_t cbuf_cap,
                          size_t *total_compressed) {
    uint64_t t0 = bench_now_ns();
    size_t in = 0, out = 0;
    while (in < cf->size) {
        size_t this_block = (cf->size - in < block) ? (cf->size - in) : block;
        if (out + sizeof(int) > cbuf_cap) return -1;
        int w;
        char *dst = (char *)cbuf + out + sizeof(int);
        int dst_cap = (int)(cbuf_cap - out - sizeof(int));
        if (level == 0) {
            w = LZ4_compress_default((const char *)cf->data + in, dst,
                                     (int)this_block, dst_cap);
        } else {
            w = LZ4_compress_HC((const char *)cf->data + in, dst,
                                (int)this_block, dst_cap, level);
        }
        if (w <= 0) {
            fprintf(stderr, "compress failure at in=%zu block=%zu level=%d\n",
                    in, this_block, level);
            return -1;
        }
        memcpy(cbuf + out, &w, sizeof(int));
        out += sizeof(int) + (size_t)w;
        in += this_block;
    }
    uint64_t t1 = bench_now_ns();
    *total_compressed = out;
    return (double)(t1 - t0);
}

/* Multi-threaded compress that mirrors ds4 PR #186's kvstore codec:
 *   - Input is split into N contiguous chunks (one per worker thread).
 *   - Each worker chunk is itself a stream of (i32 length-prefix, payload)
 *     blocks of `block` bytes each (the worker uses the same block-stream
 *     format as do_compress so the existing decompress path can verify).
 *   - Wall-clock spans pthread_create -> pthread_join (the cost ds4 pays).
 *   - Each worker writes into a private destination region; on return,
 *     workers' outputs are concatenated into cbuf. Per-worker offsets/sizes
 *     are recorded so do_decompress_mt can replay them.
 *
 * Returns wall-clock nanoseconds, or -1 on failure. */
typedef struct {
    const unsigned char *src;
    size_t               src_size;
    unsigned char       *dst;            /* private staging buffer */
    size_t               dst_cap;
    size_t               block;
    int                  level;
    size_t               compressed;     /* OUT: bytes written into dst */
    int                  err;
} mt_compress_arg_t;

static void *mt_compress_worker(void *vp) {
    mt_compress_arg_t *a = (mt_compress_arg_t *)vp;
    size_t in = 0, out = 0;
    while (in < a->src_size) {
        size_t this_block = (a->src_size - in < a->block) ? (a->src_size - in) : a->block;
        if (out + sizeof(int) > a->dst_cap) { a->err = 1; return NULL; }
        int w;
        char *dst = (char *)a->dst + out + sizeof(int);
        int dst_cap = (int)(a->dst_cap - out - sizeof(int));
        if (a->level == 0) {
            w = LZ4_compress_default((const char *)a->src + in, dst,
                                     (int)this_block, dst_cap);
        } else {
            w = LZ4_compress_HC((const char *)a->src + in, dst,
                                (int)this_block, dst_cap, a->level);
        }
        if (w <= 0) { a->err = 1; return NULL; }
        memcpy(a->dst + out, &w, sizeof(int));
        out += sizeof(int) + (size_t)w;
        in  += this_block;
    }
    a->compressed = out;
    return NULL;
}

/* Per-chunk metadata so the decompressor knows where each thread's output
 * lives in cbuf and how many decompressed bytes it represents. */
typedef struct {
    size_t in_off;        /* offset into the original corpus */
    size_t in_size;       /* uncompressed bytes this chunk covers */
    size_t out_off;       /* offset into cbuf where this chunk's compressed stream starts */
    size_t out_size;      /* compressed bytes for this chunk */
} mt_chunk_t;

static double do_compress_mt(const corpus_file_t *cf, size_t block, int level,
                             unsigned char *cbuf, size_t cbuf_cap,
                             size_t *total_compressed,
                             int threads, size_t chunk_bytes,
                             mt_chunk_t *chunks_out, int *n_chunks_out) {
    if (threads < 1) threads = 1;
    if (threads > BENCH_MAX_THREADS) threads = BENCH_MAX_THREADS;
    if (chunk_bytes < 64 * 1024) chunk_bytes = 64 * 1024;

    /* Carve corpus into chunks. The last chunk soaks up the remainder so
     * total work matches cf->size exactly. */
    int n_chunks = (int)((cf->size + chunk_bytes - 1) / chunk_bytes);
    if (n_chunks < 1) n_chunks = 1;

    /* Each worker needs a private destination so they don't tread on each
     * other. Bound it generously by LZ4_compressBound on the chunk. */
    int cap_per_chunk = LZ4_compressBound((int)chunk_bytes);
    if (cap_per_chunk <= 0) return -1;
    size_t per_chunk_dst_cap = (size_t)cap_per_chunk + (chunk_bytes / block + 1) * sizeof(int) + 128;

    mt_compress_arg_t *args = (mt_compress_arg_t *)calloc((size_t)n_chunks, sizeof(*args));
    pthread_t          *tids = (pthread_t          *)calloc((size_t)n_chunks, sizeof(*tids));
    unsigned char      *staging = (unsigned char *)malloc((size_t)n_chunks * per_chunk_dst_cap);
    if (!args || !tids || !staging) { free(args); free(tids); free(staging); return -1; }

    for (int i = 0; i < n_chunks; i++) {
        size_t off = (size_t)i * chunk_bytes;
        size_t sz  = (i == n_chunks - 1) ? (cf->size - off) : chunk_bytes;
        args[i].src      = cf->data + off;
        args[i].src_size = sz;
        args[i].dst      = staging + (size_t)i * per_chunk_dst_cap;
        args[i].dst_cap  = per_chunk_dst_cap;
        args[i].block    = block;
        args[i].level    = level;
    }

    uint64_t t0 = bench_now_ns();
    /* Pool of `threads` workers servicing `n_chunks` work items. For the
     * common case of n_chunks == threads we just spawn one each; for n_chunks
     * > threads we batch in waves so peak parallelism is `threads`. */
    int next = 0;
    while (next < n_chunks) {
        int wave = (n_chunks - next < threads) ? (n_chunks - next) : threads;
        for (int i = 0; i < wave; i++) {
            if (pthread_create(&tids[next + i], NULL, mt_compress_worker, &args[next + i]) != 0) {
                /* On failure, run remaining chunks inline (degraded but correct). */
                mt_compress_worker(&args[next + i]);
                tids[next + i] = 0;
            }
        }
        for (int i = 0; i < wave; i++) {
            if (tids[next + i]) pthread_join(tids[next + i], NULL);
        }
        next += wave;
    }
    uint64_t t1 = bench_now_ns();

    /* Concatenate workers' outputs into cbuf and emit chunk metadata. */
    size_t out_total = 0;
    for (int i = 0; i < n_chunks; i++) {
        if (args[i].err) { free(args); free(tids); free(staging); return -1; }
        if (out_total + args[i].compressed > cbuf_cap) { free(args); free(tids); free(staging); return -1; }
        memcpy(cbuf + out_total, args[i].dst, args[i].compressed);
        chunks_out[i].in_off   = (size_t)(args[i].src - cf->data);
        chunks_out[i].in_size  = args[i].src_size;
        chunks_out[i].out_off  = out_total;
        chunks_out[i].out_size = args[i].compressed;
        out_total += args[i].compressed;
    }
    *total_compressed = out_total;
    *n_chunks_out     = n_chunks;
    free(args); free(tids); free(staging);
    return (double)(t1 - t0);
}

static double do_decompress(const corpus_file_t *cf, size_t block,
                            const unsigned char *cbuf, size_t cbuf_size,
                            unsigned char *dbuf) {
    uint64_t t0 = bench_now_ns();
    size_t in = 0, out = 0;
    while (in < cbuf_size) {
        int w;
        memcpy(&w, cbuf + in, sizeof(int));
        in += sizeof(int);
        size_t this_block = (cf->size - out < block) ? (cf->size - out) : block;
        int n = LZ4_decompress_safe((const char *)cbuf + in,
                                    (char *)dbuf + out,
                                    w, (int)this_block);
        if (n < 0) {
            fprintf(stderr, "decompress failure at in=%zu w=%d block=%zu\n",
                    in, w, this_block);
            return -1;
        }
        in  += (size_t)w;
        out += (size_t)n;
    }
    uint64_t t1 = bench_now_ns();
    return (double)(t1 - t0);
}

/* Multi-threaded decompress: walks the chunk metadata table and dispatches
 * each chunk to a worker. Matches the "future: multi-threaded decompress"
 * direction noted in PR #186 - implementation-ready for when ds4 wires it
 * in, and useful here to confirm per-core opts hold up under contention. */
typedef struct {
    const unsigned char *src;
    size_t               src_size;
    unsigned char       *dst;
    size_t               dst_size;       /* uncompressed bytes expected */
    size_t               block;
    int                  err;
} mt_decompress_arg_t;

static void *mt_decompress_worker(void *vp) {
    mt_decompress_arg_t *a = (mt_decompress_arg_t *)vp;
    size_t in = 0, out = 0;
    while (in < a->src_size) {
        int w;
        memcpy(&w, a->src + in, sizeof(int));
        in += sizeof(int);
        size_t this_block = (a->dst_size - out < a->block) ? (a->dst_size - out) : a->block;
        int n = LZ4_decompress_safe((const char *)a->src + in,
                                    (char *)a->dst + out,
                                    w, (int)this_block);
        if (n < 0) { a->err = 1; return NULL; }
        in  += (size_t)w;
        out += (size_t)n;
    }
    return NULL;
}

static double do_decompress_mt(const corpus_file_t *cf, size_t block,
                               const unsigned char *cbuf, size_t cbuf_size,
                               unsigned char *dbuf,
                               int threads,
                               const mt_chunk_t *chunks, int n_chunks) {
    (void)cbuf_size;
    if (threads < 1) threads = 1;
    if (threads > BENCH_MAX_THREADS) threads = BENCH_MAX_THREADS;

    mt_decompress_arg_t *args = (mt_decompress_arg_t *)calloc((size_t)n_chunks, sizeof(*args));
    pthread_t           *tids = (pthread_t           *)calloc((size_t)n_chunks, sizeof(*tids));
    if (!args || !tids) { free(args); free(tids); return -1; }

    for (int i = 0; i < n_chunks; i++) {
        args[i].src      = cbuf + chunks[i].out_off;
        args[i].src_size = chunks[i].out_size;
        args[i].dst      = dbuf + chunks[i].in_off;
        args[i].dst_size = chunks[i].in_size;
        args[i].block    = block;
    }

    uint64_t t0 = bench_now_ns();
    int next = 0;
    while (next < n_chunks) {
        int wave = (n_chunks - next < threads) ? (n_chunks - next) : threads;
        for (int i = 0; i < wave; i++) {
            if (pthread_create(&tids[next + i], NULL, mt_decompress_worker, &args[next + i]) != 0) {
                mt_decompress_worker(&args[next + i]);
                tids[next + i] = 0;
            }
        }
        for (int i = 0; i < wave; i++) {
            if (tids[next + i]) pthread_join(tids[next + i], NULL);
        }
        next += wave;
    }
    uint64_t t1 = bench_now_ns();

    for (int i = 0; i < n_chunks; i++) {
        if (args[i].err) { free(args); free(tids); (void)cf; return -1; }
    }
    free(args); free(tids); (void)cf;
    return (double)(t1 - t0);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s (--corpus PATH | --synth TYPE:SIZE) [options]\n"
        "  --corpus PATH      load file as the input corpus\n"
        "  --synth TYPE:SIZE  generate a synthetic corpus\n"
        "                     TYPE ∈ random|ascii|repetitive|json|mixed|\n"
        "                            q4|q8|f16|bf16|kvcache\n"
        "  --block N          per-LZ4-block chunk size in bytes (default 65536)\n"
        "  --level N          0 = LZ4 fast (default), 1..12 = LZ4_HC level\n"
        "  --iters N          measurement iterations (default 11, max %d)\n"
        "  --warmup N         warmup iterations to discard (default 3)\n"
        "  --threads N        worker threads for compress + decompress (default 1).\n"
        "                     Matches the ds4 KV cache codec from PR #186.\n"
        "  --chunk-mib N      per-thread chunk size in MiB (default 16, matches PR #186)\n"
        "  --variant NAME     label for this build variant in the JSON output\n"
        "  --json             emit a single JSON line on stdout (for grid runner)\n"
        "  --quiet            suppress the stderr banner\n",
        prog, BENCH_MAX_ITERS);
}

int main(int argc, char **argv) {
    const char *corpus_path = NULL;
    const char *synth_spec = NULL;
    size_t block = 65536;
    int level = 0, iters = 11, warmup = 3;
    int threads = 1;
    size_t chunk_mib = 16;     /* per-thread chunk size, matches PR #186 default */
    const char *variant = "unknown";
    int json_out = 0, quiet = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--corpus")  && i+1 < argc) corpus_path = argv[++i];
        else if (!strcmp(a, "--synth")   && i+1 < argc) synth_spec  = argv[++i];
        else if (!strcmp(a, "--block")   && i+1 < argc) block  = (size_t)atoll(argv[++i]);
        else if (!strcmp(a, "--level")   && i+1 < argc) level  = atoi(argv[++i]);
        else if (!strcmp(a, "--iters")   && i+1 < argc) iters  = atoi(argv[++i]);
        else if (!strcmp(a, "--warmup") && i+1 < argc) warmup = atoi(argv[++i]);
        else if (!strcmp(a, "--threads") && i+1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(a, "--chunk-mib") && i+1 < argc) chunk_mib = (size_t)atoll(argv[++i]);
        else if (!strcmp(a, "--variant") && i+1 < argc) variant= argv[++i];
        else if (!strcmp(a, "--json"))  json_out = 1;
        else if (!strcmp(a, "--quiet")) quiet = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", a); usage(argv[0]); return 2; }
    }

    if ((!corpus_path && !synth_spec) || (corpus_path && synth_spec)) {
        usage(argv[0]); return 2;
    }
    if (iters > BENCH_MAX_ITERS) iters = BENCH_MAX_ITERS;
    if (iters < 1) iters = 1;
    if (warmup < 0) warmup = 0;
    if (block < 64) block = 64;
    if (threads < 1) threads = 1;
    if (threads > BENCH_MAX_THREADS) threads = BENCH_MAX_THREADS;
    if (chunk_mib < 1) chunk_mib = 1;
    size_t chunk_bytes = chunk_mib * 1024 * 1024;

    corpus_file_t cf = {0};
    if (corpus_path) {
        if (corpus_load_file(corpus_path, &cf) != 0) {
            fprintf(stderr, "failed to load %s\n", corpus_path);
            return 1;
        }
    } else {
        char buf[256];
        strncpy(buf, synth_spec, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        char *colon = strchr(buf, ':');
        if (!colon) { usage(argv[0]); return 2; }
        *colon = 0;
        size_t sz = (size_t)atoll(colon + 1);
        if (corpus_generate(buf, sz, &cf) != 0) {
            fprintf(stderr, "failed to generate synth corpus %s\n", synth_spec);
            return 1;
        }
    }

    if (!quiet) {
        fprintf(stderr,
                "# lz4-opt bench variant=%s corpus=%s size=%zu block=%zu level=%d iters=%d warmup=%d threads=%d chunk_mib=%zu\n",
                variant, cf.name, cf.size, block, level, iters, warmup, threads, chunk_mib);
        fprintf(stderr, "# opts: " LZ4_OPT_BANNER_FMT "\n", LZ4_OPT_BANNER_ARGS);
    }

    int cap_per_block = LZ4_compressBound((int)block);
    if (cap_per_block <= 0) {
        fprintf(stderr, "LZ4_compressBound failed for block=%zu\n", block);
        return 1;
    }
    size_t blocks = (cf.size + block - 1) / block;
    if (blocks == 0) blocks = 1;
    size_t cbuf_cap = ((size_t)cap_per_block + sizeof(int)) * blocks + 128;
    unsigned char *cbuf = (unsigned char *)malloc(cbuf_cap);
    unsigned char *dbuf = (unsigned char *)malloc(cf.size + 128);
    if (!cbuf || !dbuf) { fprintf(stderr, "OOM\n"); return 1; }

    /* Chunk table for the threads>1 path. n_chunks isn't known until the first
     * compress so allocate a generous upper bound based on the input size. */
    size_t max_chunks = (cf.size + chunk_bytes - 1) / chunk_bytes + 1;
    mt_chunk_t *chunks = (mt_chunk_t *)calloc(max_chunks, sizeof(*chunks));
    int n_chunks = 0;
    if (!chunks) { fprintf(stderr, "OOM (chunks)\n"); return 1; }

    /* Warmup. */
    size_t total_compressed = 0;
    for (int i = 0; i < warmup; i++) {
        double tc, td;
        if (threads > 1) {
            tc = do_compress_mt(&cf, block, level, cbuf, cbuf_cap,
                                &total_compressed, threads, chunk_bytes,
                                chunks, &n_chunks);
            if (tc < 0) return 1;
            td = do_decompress_mt(&cf, block, cbuf, total_compressed, dbuf,
                                  threads, chunks, n_chunks);
        } else {
            tc = do_compress(&cf, block, level, cbuf, cbuf_cap, &total_compressed);
            if (tc < 0) return 1;
            td = do_decompress(&cf, block, cbuf, total_compressed, dbuf);
        }
        if (td < 0) return 1;
    }
    if (cf.size > 0 && memcmp(cf.data, dbuf, cf.size) != 0) {
        fprintf(stderr, "ROUND-TRIP MISMATCH in warmup\n");
        return 1;
    }

    samples_t cs = {.n = 0}, ds = {.n = 0};
    for (int i = 0; i < iters; i++) {
        double t;
        if (threads > 1) {
            t = do_compress_mt(&cf, block, level, cbuf, cbuf_cap,
                               &total_compressed, threads, chunk_bytes,
                               chunks, &n_chunks);
        } else {
            t = do_compress(&cf, block, level, cbuf, cbuf_cap, &total_compressed);
        }
        if (t < 0) return 1;
        cs.v[cs.n++] = t;
    }
    for (int i = 0; i < iters; i++) {
        double t;
        if (threads > 1) {
            t = do_decompress_mt(&cf, block, cbuf, total_compressed, dbuf,
                                 threads, chunks, n_chunks);
        } else {
            t = do_decompress(&cf, block, cbuf, total_compressed, dbuf);
        }
        if (t < 0) return 1;
        ds.v[ds.n++] = t;
    }
    if (cf.size > 0 && memcmp(cf.data, dbuf, cf.size) != 0) {
        fprintf(stderr, "ROUND-TRIP MISMATCH after measurement\n");
        return 1;
    }
    free(chunks);

    double c_med, c_min, c_max, c_sd;
    double d_med, d_min, d_max, d_sd;
    summarize(&cs, &c_med, &c_min, &c_max, &c_sd);
    summarize(&ds, &d_med, &d_min, &d_max, &d_sd);

    double ratio = (cf.size > 0 && total_compressed > 0)
                   ? (double)cf.size / (double)total_compressed
                   : 0.0;

    if (json_out) {
        printf("{");
        printf("\"variant\":\"%s\",", variant);
        printf("\"corpus\":\"%s\",", cf.name);
        printf("\"size\":%zu,", cf.size);
        printf("\"block\":%zu,", block);
        printf("\"level\":%d,", level);
        printf("\"threads\":%d,", threads);
        printf("\"chunk_mib\":%zu,", chunk_mib);
        printf("\"iters\":%d,", iters);
        printf("\"compressed\":%zu,", total_compressed);
        printf("\"ratio\":%.4f,", ratio);
        /* min ns ==> max MB/s (fastest single iteration). */
        printf("\"compress_mbps_med\":%.2f,", mbps(cf.size, c_med));
        printf("\"compress_mbps_best\":%.2f,", mbps(cf.size, c_min));
        printf("\"compress_mbps_worst\":%.2f,", mbps(cf.size, c_max));
        printf("\"compress_ns_med\":%.0f,", c_med);
        printf("\"compress_ns_sd\":%.0f,", c_sd);
        printf("\"decompress_mbps_med\":%.2f,", mbps(cf.size, d_med));
        printf("\"decompress_mbps_best\":%.2f,", mbps(cf.size, d_min));
        printf("\"decompress_mbps_worst\":%.2f,", mbps(cf.size, d_max));
        printf("\"decompress_ns_med\":%.0f,", d_med);
        printf("\"decompress_ns_sd\":%.0f,", d_sd);
        printf("\"opts\":\"" LZ4_OPT_BANNER_FMT "\"", LZ4_OPT_BANNER_ARGS);
        printf("}\n");
    } else {
        printf("variant=%s corpus=%s size=%zu block=%zu level=%d\n",
               variant, cf.name, cf.size, block, level);
        printf("  compressed=%zu ratio=%.4f\n", total_compressed, ratio);
        printf("  compress    median=%8.1f MB/s   best=%8.1f   worst=%8.1f   sd_ns=%.0f\n",
               mbps(cf.size, c_med), mbps(cf.size, c_min), mbps(cf.size, c_max), c_sd);
        printf("  decompress  median=%8.1f MB/s   best=%8.1f   worst=%8.1f   sd_ns=%.0f\n",
               mbps(cf.size, d_med), mbps(cf.size, d_min), mbps(cf.size, d_max), d_sd);
    }

    free(cbuf); free(dbuf);
    corpus_free(&cf);
    return 0;
}
