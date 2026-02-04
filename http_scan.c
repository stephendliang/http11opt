#include <immintrin.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_cycles.h"

typedef struct {
    __mmask64 sp[2];    /* space 0x20 */
    __mmask64 cr[2];    /* CR 0x0D */
    __mmask64 lf[2];    /* LF 0x0A */
    __mmask64 col[2];   /* colon 0x3A */
    __mmask64 alpha[2]; /* A-Za-z */
} http_tok_t;

static inline __attribute__((always_inline, hot)) int
http_scan_ws_loop(const char *__restrict__ buf, size_t len,
                  http_tok_t *__restrict__ t, size_t *__restrict__ pos)
{
    const __m512i vsp  = _mm512_set1_epi8(0x20); /* space AND case-fold bit */
    const __m512i vcr  = _mm512_set1_epi8('\r');
    const __m512i vlf  = _mm512_set1_epi8('\n');
    const __m512i vcol = _mm512_set1_epi8(':');
    const __m512i va   = _mm512_set1_epi8('a');
    const __m512i v26  = _mm512_set1_epi8(26);

    for (size_t i = 0; i < len; i += 128) {
        const __m512i d0 = _mm512_load_si512((const __m512i *)(buf + i));
        const __m512i d1 = _mm512_load_si512((const __m512i *)(buf + i + 64));

        t->sp[0]  = _mm512_cmpeq_epi8_mask(d0, vsp);
        t->sp[1]  = _mm512_cmpeq_epi8_mask(d1, vsp);
        t->cr[0]  = _mm512_cmpeq_epi8_mask(d0, vcr);
        t->cr[1]  = _mm512_cmpeq_epi8_mask(d1, vcr);
        t->lf[0]  = _mm512_cmpeq_epi8_mask(d0, vlf);
        t->lf[1]  = _mm512_cmpeq_epi8_mask(d1, vlf);
        t->col[0] = _mm512_cmpeq_epi8_mask(d0, vcol);
        t->col[1] = _mm512_cmpeq_epi8_mask(d1, vcol);

        /* alpha: OR 0x20 folds A-Z to a-z, then range check [a, a+26) */
        __m512i f0 = _mm512_or_si512(d0, vsp);
        __m512i f1 = _mm512_or_si512(d1, vsp);
        __m512i s0 = _mm512_sub_epi8(f0, va);
        __m512i s1 = _mm512_sub_epi8(f1, va);
        t->alpha[0] = _mm512_cmplt_epu8_mask(s0, v26);
        t->alpha[1] = _mm512_cmplt_epu8_mask(s1, v26);

        __mmask64 any0 = t->sp[0] | t->cr[0] | t->lf[0] | t->col[0] | t->alpha[0];
        __mmask64 any1 = t->sp[1] | t->cr[1] | t->lf[1] | t->col[1] | t->alpha[1];

        if (__builtin_expect(any0 | any1, 1)) {
            *pos = i;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    size_t flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t padded = (flen + 127) & ~127UL;
    char *buf = aligned_alloc(64, padded);
    memset(buf, 0, padded);
    fread(buf, 1, flen, f);
    fclose(f);

    http_tok_t t;
    size_t pos = 0;

    uint64_t t0 = rdtsc_start();
    int found = http_scan_ws_loop(buf, padded, &t, &pos);
    uint64_t t1 = rdtsc_end();

    uint64_t cycles = t1 - t0;

    printf("len=%zu padded=%zu found=%d pos=%zu cycles=%lu\n",
           flen, padded, found, pos, cycles);
    if (found) {
        printf("sp[0]=%016llx sp[1]=%016llx\n", (unsigned long long)t.sp[0], (unsigned long long)t.sp[1]);
        printf("cr[0]=%016llx cr[1]=%016llx\n", (unsigned long long)t.cr[0], (unsigned long long)t.cr[1]);
        printf("lf[0]=%016llx lf[1]=%016llx\n", (unsigned long long)t.lf[0], (unsigned long long)t.lf[1]);
        printf("col[0]=%016llx col[1]=%016llx\n", (unsigned long long)t.col[0], (unsigned long long)t.col[1]);
        printf("alpha[0]=%016llx alpha[1]=%016llx\n", (unsigned long long)t.alpha[0], (unsigned long long)t.alpha[1]);
    }

    free(buf);
    return 0;
}
