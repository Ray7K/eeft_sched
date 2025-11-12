#ifndef LIB_MATH_H
#define LIB_MATH_H

#include <stdint.h>
#include <stdlib.h>

static inline uint32_t gcd(uint32_t a, uint32_t b) {
  while (b != 0) {
    uint32_t temp = b;
    b = a % b;
    a = temp;
  }
  return a;
}

static inline uint32_t lcm(uint32_t a, uint32_t b) {
  if (a == 0 || b == 0)
    return 0;
  return (a / gcd(a, b)) * b;
}

static inline uint32_t safe_lcm(uint32_t a, uint32_t b, uint32_t cap) {
  if (a == 0 || b == 0)
    return 0;

  uint64_t result = (uint64_t)a / gcd(a, b) * b;
  if (result > cap)
    return cap;
  return (uint32_t)result;
}

static int cmp_uint32(const void *a, const void *b) {
  uint32_t x = *(uint32_t *)a, y = *(uint32_t *)b;
  return (x > y) - (x < y);
}

static inline float rand_between(float min, float max) {
  return min + (float)rand() / (float)RAND_MAX * (max - min);
}

#endif
