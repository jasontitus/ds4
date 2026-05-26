#ifndef BENCH_CORPUS_H
#define BENCH_CORPUS_H

#include <stddef.h>

typedef struct {
    char *name;
    unsigned char *data;
    size_t size;
    int owns_data;
} corpus_file_t;

/* Load a file into memory. */
int corpus_load_file(const char *path, corpus_file_t *out);

/* Generate a deterministic synthetic corpus.
 * type ∈ {"random","ascii","repetitive","json","mixed",
 *        "q4","q8","f16","bf16","kvcache"}.
 *
 * The latter five mimic ds4 byte shapes:
 *   q4      - blocks of fp16 scale + 4-bit packed quants
 *   q8      - blocks of fp16 scale + 8-bit quants
 *   f16     - small-magnitude fp16 activations
 *   bf16    - small-magnitude bf16 activations
 *   kvcache - block-structured rows with header + Q8 + fp16 + Q4 + optional zero run
 */
int corpus_generate(const char *type, size_t size, corpus_file_t *out);

void corpus_free(corpus_file_t *cf);

#endif
