/*
 * wide.h: a 64-bit number times a 32-bit one, exactly (96 bits), compared
 * and divided, in 64-bit integers: the same on every processor.
 */
#ifndef PORT_WIDE_H
#define PORT_WIDE_H

#include <stdint.h>

typedef struct {
    uint64_t hi;               /* the product is hi << 32 | lo */
    uint32_t lo;
} Wide;

static inline Wide wide_mul(uint64_t a, uint32_t b)
{
    /* (a >> 32) b is at most (2^32 - 1)^2, so hi takes l's carry */
    uint64_t l = (uint64_t)(uint32_t)a * b;
    Wide w;
    w.hi = (a >> 32) * b + (l >> 32);
    w.lo = (uint32_t)l;
    return w;
}

static inline int wide_less(Wide x, Wide y) { return x.hi < y.hi || (x.hi == y.hi && x.lo < y.lo); }

/* x / d's low 32 bits (d > 0): hi = q d + r, and (r << 32 | lo) / d is
   the quotient's low part, below 2^32 as r < d */
static inline uint32_t wide_div_lo(Wide x, uint32_t d)
{
    uint64_t r = x.hi % d;
    return (uint32_t)((r << 32 | x.lo) / d);
}

#endif
