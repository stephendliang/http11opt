#ifndef CLOCK_CYCLES_H
#define CLOCK_CYCLES_H

#include <stdint.h>

/* CPUID serializes the pipeline, RDTSC reads timestamp counter */
static inline __attribute__((always_inline)) uint64_t rdtsc_start(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__ (
        "cpuid\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        : "a"(0)
        : "rbx", "rcx"
    );
    return ((uint64_t)hi << 32) | lo;
}

/* RDTSCP waits for prior instructions, CPUID prevents reordering with later ops */
static inline __attribute__((always_inline)) uint64_t rdtsc_end(void)
{
    uint32_t lo, hi, aux;
    __asm__ __volatile__ (
        "rdtscp\n\t"
        "mov %%eax, %0\n\t"
        "mov %%edx, %1\n\t"
        "cpuid"
        : "=r"(lo), "=r"(hi), "=c"(aux)
        :
        : "rax", "rbx", "rdx"
    );
    return ((uint64_t)hi << 32) | lo;
}

#endif /* CLOCK_CYCLES_H */
