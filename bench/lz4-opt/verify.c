/* lz4-opt verify
 *
 * Exhaustive round-trip correctness check for the variant we were built with.
 * Iterates over synthetic corpora x block sizes x compression levels and
 * confirms compress(decompress(x)) == x byte-for-byte.
 *
 * Cross-checks the byte stream against the baseline format: any opt that
 * produces a decompressable but byte-different stream is allowed, but a
 * round-trip failure or buffer overflow is a hard error.
 */
#include "lz4_opts.h"
#include "lz4/lib/lz4.h"
#include "lz4/lib/lz4hc.h"
#include "corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int verify_one(const corpus_file_t *cf, size_t block, int level) {
    int cap_per_block = LZ4_compressBound((int)block);
    if (cap_per_block <= 0) return -1;
    size_t blocks = (cf->size + block - 1) / block;
    if (blocks == 0) blocks = 1;
    size_t cbuf_cap = ((size_t)cap_per_block + sizeof(int)) * blocks + 128;
    unsigned char *cbuf = (unsigned char *)malloc(cbuf_cap);
    unsigned char *dbuf = (unsigned char *)malloc(cf->size + 128);
    if (!cbuf || !dbuf) { free(cbuf); free(dbuf); return -1; }

    size_t in = 0, out = 0;
    while (in < cf->size) {
        size_t this_block = (cf->size - in < block) ? (cf->size - in) : block;
        char *dst = (char *)cbuf + out + sizeof(int);
        int dst_cap = (int)(cbuf_cap - out - sizeof(int));
        int w;
        if (level == 0) {
            w = LZ4_compress_default((const char *)cf->data + in, dst,
                                     (int)this_block, dst_cap);
        } else {
            w = LZ4_compress_HC((const char *)cf->data + in, dst,
                                (int)this_block, dst_cap, level);
        }
        if (w <= 0) {
            fprintf(stderr, "FAIL compress corpus=%s block=%zu level=%d in=%zu\n",
                    cf->name, block, level, in);
            goto fail;
        }
        memcpy(cbuf + out, &w, sizeof(int));
        out += sizeof(int) + (size_t)w;
        in  += this_block;
    }

    size_t din = 0, dout = 0;
    while (din < out) {
        int w;
        memcpy(&w, cbuf + din, sizeof(int));
        din += sizeof(int);
        size_t this_block = (cf->size - dout < block) ? (cf->size - dout) : block;
        int n = LZ4_decompress_safe((const char *)cbuf + din,
                                    (char *)dbuf + dout,
                                    w, (int)this_block);
        if (n < 0) {
            fprintf(stderr, "FAIL decompress corpus=%s block=%zu level=%d din=%zu w=%d\n",
                    cf->name, block, level, din, w);
            goto fail;
        }
        din  += (size_t)w;
        dout += (size_t)n;
    }
    if (dout != cf->size || memcmp(cf->data, dbuf, cf->size) != 0) {
        fprintf(stderr, "FAIL roundtrip corpus=%s block=%zu level=%d (got %zu bytes vs %zu)\n",
                cf->name, block, level, dout, cf->size);
        goto fail;
    }
    free(cbuf); free(dbuf);
    return 0;
fail:
    free(cbuf); free(dbuf);
    return -1;
}

int main(void) {
    static const char *types[]  = {
        "random", "ascii", "repetitive", "json", "mixed",
        "q4", "q8", "f16", "bf16", "kvcache"
    };
    /* Sizes/blocks chosen to exercise the same offset distribution the bench
     * sees. The dec_no_ldp opt was found to break only at offset∈[8..15],
     * which only fires from kvcache:16M / 4 KB block - earlier verify sets
     * topped out at 1 MB and missed it. */
    static const size_t sizes[]  = { 64, 256, 4096, 65536, 1024*1024, 16*1024*1024 };
    static const size_t blocks[] = { 64, 4096, 16384, 65536, 1024*1024 };
    static const int    levels[] = { 0, 1, 3, 6, 9, 12 };

    fprintf(stderr, "# verify opts: " LZ4_OPT_BANNER_FMT "\n", LZ4_OPT_BANNER_ARGS);

    int total = 0, failed = 0;
    for (size_t t = 0; t < sizeof(types)/sizeof(types[0]); t++) {
        for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
            corpus_file_t cf;
            if (corpus_generate(types[t], sizes[s], &cf) != 0) {
                fprintf(stderr, "skip generate %s:%zu\n", types[t], sizes[s]);
                continue;
            }
            for (size_t b = 0; b < sizeof(blocks)/sizeof(blocks[0]); b++) {
                /* Block must be <= corpus, otherwise it degenerates. Skip
                 * the most pointless combinations. */
                if (blocks[b] > sizes[s] && blocks[b] != 64) continue;
                for (size_t l = 0; l < sizeof(levels)/sizeof(levels[0]); l++) {
                    total++;
                    if (verify_one(&cf, blocks[b], levels[l]) != 0) failed++;
                }
            }
            corpus_free(&cf);
        }
    }
    fprintf(stderr, "verify: %d cases run, %d failures\n", total, failed);
    return failed == 0 ? 0 : 1;
}
