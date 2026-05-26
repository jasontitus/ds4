#include "corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int corpus_load_file(const char *path, corpus_file_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    unsigned char *buf = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    out->name = xstrdup(path);
    out->data = buf;
    out->size = (size_t)sz;
    out->owns_data = 1;
    return 0;
}

/* Stable xorshift64 so synthetic corpora are byte-identical across runs and
 * across machines. */
static uint64_t xs64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static int gen_random(unsigned char *buf, size_t size, uint64_t seed) {
    uint64_t s = seed;
    size_t i = 0;
    while (i + 8 <= size) {
        uint64_t v = xs64(&s);
        memcpy(buf + i, &v, 8);
        i += 8;
    }
    while (i < size) {
        buf[i++] = (unsigned char)(xs64(&s) & 0xff);
    }
    return 0;
}

static int gen_ascii(unsigned char *buf, size_t size, uint64_t seed) {
    /* English-text-shaped weighting: spaces, common letters, occasional
     * punctuation. Designed to be LZ4-friendly with long-ish matches. */
    static const char alpha[] =
        "                    "
        "eeeeeeeeeeeettttttttaaaaaaaooooooonnnnnnniiiiiiisssssrrrrrhhhhldddcccuummffggwwyy"
        "AbpvkjxqzABCDEFGHIJKLMNOPQRSTUVWXYZ.,!?'\"\n";
    size_t n = sizeof(alpha) - 1;
    uint64_t s = seed;
    for (size_t i = 0; i < size; i++) {
        buf[i] = (unsigned char)alpha[xs64(&s) % n];
    }
    return 0;
}

static int gen_repetitive(unsigned char *buf, size_t size, uint64_t seed) {
    /* Lots of runs of varying length: stresses small-offset RLE paths. */
    uint64_t s = seed;
    size_t i = 0;
    while (i < size) {
        unsigned char v = (unsigned char)(xs64(&s) & 0xff);
        size_t run = 4 + (size_t)(xs64(&s) % 60);
        if (i + run > size) run = size - i;
        memset(buf + i, v, run);
        i += run;
    }
    return 0;
}

static int gen_json(unsigned char *buf, size_t size, uint64_t seed) {
    static const char *keys[] = {
        "id","name","value","ts","user","data","ok","error","next","items"
    };
    uint64_t s = seed;
    size_t off = 0;
    while (off < size) {
        int k = (int)(xs64(&s) % 10);
        int v = (int)(xs64(&s) % 1000000);
        int kn = (k + 1) % 10;
        int vn = (k + 2) % 10;
        int n = snprintf((char *)buf + off, size - off,
                         "{\"%s\":%d,\"%s\":\"%s_%d\"}\n",
                         keys[k], v, keys[kn], keys[vn], v);
        if (n <= 0 || (size_t)n > size - off) {
            memset(buf + off, ' ', size - off);
            break;
        }
        off += (size_t)n;
    }
    return 0;
}

/* -- ds4-style synthetic corpora --
 * These approximate the byte shapes that show up in ds4 KV cache and weight
 * files: quantized integer streams with periodic fp16 scales, raw fp16/bf16
 * activations weighted toward small magnitudes, and a block-structured
 * "kvcache" blend that nests all of the above behind regular headers. */

/* Q4-style: blocks of [fp16 scale | 16 bytes packed signed 4-bit].
 * Values weighted toward zero to mimic post-quantization distributions. */
static int gen_q4(unsigned char *buf, size_t size, uint64_t seed) {
    uint64_t s = seed;
    size_t off = 0;
    while (off + 18 <= size) {
        uint16_t scale = (uint16_t)(((xs64(&s) & 0x03FF) | 0x1C00));
        memcpy(buf + off, &scale, 2);
        off += 2;
        for (int i = 0; i < 16; i++) {
            uint64_t r = xs64(&s);
            int lo = (int)((r        & 0xFF) % 11) - 5;
            int hi = (int)(((r >> 8) & 0xFF) % 11) - 5;
            buf[off++] = (unsigned char)((((unsigned)hi & 0xF) << 4) | ((unsigned)lo & 0xF));
        }
    }
    while (off < size) buf[off++] = 0;
    return 0;
}

/* Q8-style: blocks of [fp16 scale | 32 signed 8-bit values].
 * Sum-of-4-uniforms gives a near-normal distribution. */
static int gen_q8(unsigned char *buf, size_t size, uint64_t seed) {
    uint64_t s = seed;
    size_t off = 0;
    while (off + 34 <= size) {
        uint16_t scale = (uint16_t)(((xs64(&s) & 0x03FF) | 0x1C00));
        memcpy(buf + off, &scale, 2);
        off += 2;
        for (int i = 0; i < 32; i++) {
            int v = 0;
            for (int j = 0; j < 4; j++) v += (int)(xs64(&s) & 0xFF);
            v = (v >> 2) - 128;
            buf[off++] = (unsigned char)(v & 0xFF);
        }
    }
    while (off < size) buf[off++] = 0;
    return 0;
}

/* Raw fp16 activation-ish stream: small magnitudes, ~10% explicit zeros,
 * exponents tightly clustered. The "top byte" (sign+exp) carries most of
 * the LZ4-visible repetition. */
static int gen_f16(unsigned char *buf, size_t size, uint64_t seed) {
    uint64_t s = seed;
    size_t i = 0;
    while (i + 2 <= size) {
        uint64_t r = xs64(&s);
        uint16_t val;
        if ((r & 0xFF) < 25) {
            val = 0;
        } else {
            uint16_t sign     = (uint16_t)((r >> 15) & 1u);
            uint16_t exp_bias = (uint16_t)(((r >> 16) % 11u) + 5u);   /* 5..15 */
            uint16_t mant     = (uint16_t)(r & 0x3FFu);
            val = (uint16_t)((sign << 15) | (exp_bias << 10) | mant);
        }
        memcpy(buf + i, &val, 2);
        i += 2;
    }
    while (i < size) buf[i++] = 0;
    return 0;
}

/* Raw bf16 activation-ish stream: exponent biased near 127 (~1.0). */
static int gen_bf16(unsigned char *buf, size_t size, uint64_t seed) {
    uint64_t s = seed;
    size_t i = 0;
    while (i + 2 <= size) {
        uint64_t r = xs64(&s);
        uint16_t val;
        if ((r & 0xFF) < 25) {
            val = 0;
        } else {
            uint16_t sign = (uint16_t)((r >> 15) & 1u);
            uint16_t exp  = (uint16_t)(((r >> 16) % 20u) + 120u);  /* 120..139 */
            uint16_t mant = (uint16_t)(r & 0x7Fu);
            val = (uint16_t)((sign << 15) | (exp << 7) | mant);
        }
        memcpy(buf + i, &val, 2);
        i += 2;
    }
    while (i < size) buf[i++] = 0;
    return 0;
}

/* ds4-shaped KV cache approximation: rows consist of
 *   [32-byte header] [Q8 KV] [fp16 indexer] [Q4 weights] [optional zero run]
 * with regular header constants and varying body sizes. */
static int gen_kvcache(unsigned char *buf, size_t size, uint64_t seed) {
    uint64_t s = seed;
    size_t off = 0;
    uint32_t row = 0;
    while (off + 32 + 1024 + 512 + 1024 <= size) {
        uint32_t hdr[8];
        hdr[0] = 0xDEADBEEFu;
        hdr[1] = row;
        hdr[2] = row * 4u;
        hdr[3] = (uint32_t)(xs64(&s) & 0xFFFFu);
        hdr[4] = 0x00000001u;
        hdr[5] = 0u;
        hdr[6] = (uint32_t)(xs64(&s) & 0xFFFFu);
        hdr[7] = 0xCAFEBABEu;
        memcpy(buf + off, hdr, 32);
        off += 32;
        gen_q8 (buf + off, 1024, s ^ 0x1111u); s = xs64(&s); off += 1024;
        gen_f16(buf + off,  512, s ^ 0x2222u); s = xs64(&s); off +=  512;
        gen_q4 (buf + off, 1024, s ^ 0x3333u); s = xs64(&s); off += 1024;
        if ((xs64(&s) & 0x7u) == 0u) {
            size_t zsz = 64 + (size_t)(xs64(&s) % 256u);
            if (off + zsz > size) zsz = size - off;
            memset(buf + off, 0, zsz);
            off += zsz;
        }
        row++;
    }
    /* Pad remainder with raw bf16-ish so the tail of the corpus also has
     * some non-trivial structure for the bench to encounter. */
    if (off < size) gen_bf16(buf + off, size - off, s ^ 0x4444u);
    return 0;
}

static int gen_mixed(unsigned char *buf, size_t size, uint64_t seed) {
    /* Four equal regions: random, ascii, repetitive, json. */
    size_t q = size / 4;
    if (q == 0) return gen_random(buf, size, seed);
    int r;
    if ((r = gen_random(buf,         q,                seed + 1)) != 0) return r;
    if ((r = gen_ascii (buf + q,     q,                seed + 2)) != 0) return r;
    if ((r = gen_repetitive(buf+2*q, q,                seed + 3)) != 0) return r;
    if ((r = gen_json  (buf + 3*q,   size - 3*q,       seed + 4)) != 0) return r;
    return 0;
}

int corpus_generate(const char *type, size_t size, corpus_file_t *out) {
    unsigned char *buf = (unsigned char *)malloc(size > 0 ? size : 1);
    if (!buf) return -1;
    uint64_t seed = 0x123456789abcdef0ULL;
    int rc;
    if (!strcmp(type, "random"))          rc = gen_random(buf, size, seed);
    else if (!strcmp(type, "ascii"))      rc = gen_ascii(buf, size, seed);
    else if (!strcmp(type, "repetitive")) rc = gen_repetitive(buf, size, seed);
    else if (!strcmp(type, "json"))       rc = gen_json(buf, size, seed);
    else if (!strcmp(type, "mixed"))      rc = gen_mixed(buf, size, seed);
    else if (!strcmp(type, "q4"))         rc = gen_q4(buf, size, seed);
    else if (!strcmp(type, "q8"))         rc = gen_q8(buf, size, seed);
    else if (!strcmp(type, "f16"))        rc = gen_f16(buf, size, seed);
    else if (!strcmp(type, "bf16"))       rc = gen_bf16(buf, size, seed);
    else if (!strcmp(type, "kvcache"))    rc = gen_kvcache(buf, size, seed);
    else { free(buf); return -1; }
    if (rc != 0) { free(buf); return rc; }
    char namebuf[64];
    snprintf(namebuf, sizeof(namebuf), "synth:%s:%zu", type, size);
    out->name = xstrdup(namebuf);
    out->data = buf;
    out->size = size;
    out->owns_data = 1;
    return 0;
}

void corpus_free(corpus_file_t *cf) {
    if (!cf) return;
    if (cf->owns_data) free(cf->data);
    free(cf->name);
    cf->data = NULL;
    cf->name = NULL;
    cf->size = 0;
}
