/* Tiny utility: generate one of the synthetic corpora and write it to a file
 * so external benchmark tools (e.g. upstream `lz4 -b`) can be pointed at the
 * same byte distributions our framework benches. */
#include "corpus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s TYPE SIZE OUTFILE\n", argv[0]);
        fprintf(stderr, "  TYPE ∈ random|ascii|repetitive|json|mixed|q4|q8|f16|bf16|kvcache\n");
        return 2;
    }
    size_t sz = (size_t)atoll(argv[2]);
    corpus_file_t cf = {0};
    if (corpus_generate(argv[1], sz, &cf) != 0) {
        fprintf(stderr, "generate failed for %s:%zu\n", argv[1], sz);
        return 1;
    }
    FILE *f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); corpus_free(&cf); return 1; }
    if (fwrite(cf.data, 1, cf.size, f) != cf.size) {
        perror("fwrite"); fclose(f); corpus_free(&cf); return 1;
    }
    fclose(f);
    fprintf(stderr, "wrote %zu bytes to %s\n", cf.size, argv[3]);
    corpus_free(&cf);
    return 0;
}
