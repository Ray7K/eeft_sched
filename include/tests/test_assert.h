#ifndef TEST_ASSERT_H
#define TEST_ASSERT_H

#include "tests/test_core.h"
#include "tests/test_log.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_SKIP(ctx, msg, ...)                                               \
  do {                                                                         \
    test_log(ctx, "TEST SKIPPED: %s:%d: " msg "\n", __FILE__, __LINE__,        \
             ##__VA_ARGS__);                                                   \
    (ctx)->status = TEST_SKIP;                                                 \
    return;                                                                    \
  } while (0)

#define ASSERT(ctx, cond)                                                      \
  do {                                                                         \
    (ctx)->assertions_executed++;                                              \
    if (!(cond)) {                                                             \
      test_log((ctx), "ASSERT FAILED: %s:%d: %s\n", __FILE__, __LINE__,        \
               #cond);                                                         \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT(ctx, cond)                                                      \
  do {                                                                         \
    (ctx)->expects_executed++;                                                 \
    if (!(cond)) {                                                             \
      test_log((ctx), "EXPECT FAILED: %s:%d: %s\n", __FILE__, __LINE__,        \
               #cond);                                                         \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_RET(ctx, cond, retval, msg, ...)                                \
  do {                                                                         \
    (ctx)->assertions_executed++;                                              \
    if (!(cond)) {                                                             \
      test_log((ctx), "ASSERT FAILED: %s:%d: " msg "\n", __FILE__, __LINE__,   \
               ##__VA_ARGS__);                                                 \
      (ctx)->status = TEST_FAIL;                                               \
      return (retval);                                                         \
    }                                                                          \
  } while (0)

#define ASSERT_TRUE(ctx, cond) ASSERT(ctx, (cond))
#define ASSERT_FALSE(ctx, cond) ASSERT(ctx, !(cond))
#define EXPECT_TRUE(ctx, cond) EXPECT(ctx, (cond))
#define EXPECT_FALSE(ctx, cond) EXPECT(ctx, !(cond))

#define ASSERT_EQ(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->assertions_executed++;                                              \
    if (_va != _vb) {                                                          \
      test_log((ctx),                                                          \
               "ASSERT_EQ FAILED: %s:%d: %s == %s (got %lld vs %lld)\n",       \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_EQ(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->expects_executed++;                                                 \
    if (_va != _vb) {                                                          \
      test_log((ctx),                                                          \
               "EXPECT_EQ FAILED: %s:%d: %s == %s (got %lld vs %lld)\n",       \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_NE(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->assertions_executed++;                                              \
    if (_va == _vb) {                                                          \
      test_log((ctx), "ASSERT_NE FAILED: %s:%d: %s != %s (both %lld)\n",       \
               __FILE__, __LINE__, #a, #b, (long long)_va);                    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_NE(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->expects_executed++;                                                 \
    if (_va == _vb) {                                                          \
      test_log((ctx), "EXPECT_NE FAILED: %s:%d: %s != %s (both %lld)\n",       \
               __FILE__, __LINE__, #a, #b, (long long)_va);                    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_LT(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->assertions_executed++;                                              \
    if (!(_va < _vb)) {                                                        \
      test_log(ctx, "ASSERT_LT FAILED: %s:%d: %s < %s (got %lld, %lld)\n",     \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_LE(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->assertions_executed++;                                              \
    if (!(_va <= _vb)) {                                                       \
      test_log(ctx, "ASSERT_LE FAILED: %s:%d: %s <= %s (got %lld, %lld)\n",    \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_GT(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->assertions_executed++;                                              \
    if (!(_va > _vb)) {                                                        \
      test_log(ctx, "ASSERT_GT FAILED: %s:%d: %s > %s (got %lld, %lld)\n",     \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_GE(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->assertions_executed++;                                              \
    if (!(_va >= _vb)) {                                                       \
      test_log(ctx, "ASSERT_GE FAILED: %s:%d: %s >= %s (got %lld, %lld)\n",    \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_LT(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->expects_executed++;                                                 \
    if (!(_va < _vb)) {                                                        \
      test_log(ctx, "EXPECT_LT FAILED: %s:%d: %s < %s (got %lld, %lld)\n",     \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define EXPECT_LE(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->expects_executed++;                                                 \
    if (!(_va <= _vb)) {                                                       \
      test_log(ctx, "EXPECT_LE FAILED: %s:%d: %s <= %s (got %lld, %lld)\n",    \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define EXPECT_GT(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->expects_executed++;                                                 \
    if (!(_va > _vb)) {                                                        \
      test_log(ctx, "EXPECT_GT FAILED: %s:%d: %s > %s (got %lld, %lld)\n",     \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define EXPECT_GE(ctx, a, b)                                                   \
  do {                                                                         \
    __auto_type _va = (a);                                                     \
    __auto_type _vb = (b);                                                     \
    (ctx)->expects_executed++;                                                 \
    if (!(_va >= _vb)) {                                                       \
      test_log(ctx, "EXPECT_GE FAILED: %s:%d: %s >= %s (got %lld, %lld)\n",    \
               __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb);    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_STREQ(ctx, s1, s2)                                              \
  do {                                                                         \
    const char *_a = (s1);                                                     \
    const char *_b = (s2);                                                     \
    (ctx)->assertions_executed++;                                              \
    if (!_a || !_b || strcmp(_a, _b) != 0) {                                   \
      test_log(ctx, "ASSERT_STREQ FAILED: %s:%d: \"%s\" != \"%s\"\n",          \
               __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)");    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_STREQ(ctx, s1, s2)                                              \
  do {                                                                         \
    const char *_a = (s1);                                                     \
    const char *_b = (s2);                                                     \
    (ctx)->expects_executed++;                                                 \
    if (!_a || !_b || strcmp(_a, _b) != 0) {                                   \
      test_log(ctx, "EXPECT_STREQ FAILED: %s:%d: \"%s\" != \"%s\"\n",          \
               __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)");    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_STRCONTAINS(ctx, hay, needle)                                   \
  do {                                                                         \
    const char *_h = (hay), *_n = (needle);                                    \
    (ctx)->assertions_executed++;                                              \
    if (!_h || !_n || !strstr(_h, _n)) {                                       \
      test_log(ctx,                                                            \
               "ASSERT_STRCONTAINS FAILED: %s:%d: \"%s\" does not contain "    \
               "\"%s\"\n",                                                     \
               __FILE__, __LINE__, _h ? _h : "(null)", _n ? _n : "(null)");    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_STRCONTAINS(ctx, hay, needle)                                   \
  do {                                                                         \
    const char *_h = (hay), *_n = (needle);                                    \
    (ctx)->expects_executed++;                                                 \
    if (!_h || !_n || !strstr(_h, _n)) {                                       \
      test_log(ctx,                                                            \
               "EXPECT_STRCONTAINS FAILED: %s:%d: \"%s\" does not contain "    \
               "\"%s\"\n",                                                     \
               __FILE__, __LINE__, _h ? _h : "(null)", _n ? _n : "(null)");    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_NEAR(ctx, a, b, eps)                                            \
  do {                                                                         \
    double _va = (a), _vb = (b), _eps = (eps);                                 \
    (ctx)->assertions_executed++;                                              \
    if (fabs(_va - _vb) > _eps) {                                              \
      test_log(ctx,                                                            \
               "ASSERT_NEAR FAILED: %s:%d: |%s - %s| > %s (%.6f vs %.6f)\n",   \
               __FILE__, __LINE__, #a, #b, #eps, _va, _vb);                    \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_NEAR(ctx, a, b, eps)                                            \
  do {                                                                         \
    double _va = (a), _vb = (b), _eps = (eps);                                 \
    (ctx)->expects_executed++;                                                 \
    if (fabs(_va - _vb) > _eps) {                                              \
      test_log(ctx,                                                            \
               "EXPECT_NEAR FAILED: %s:%d: |%s - %s| > %s (%.6f vs %.6f)\n",   \
               __FILE__, __LINE__, #a, #b, #eps, _va, _vb);                    \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_NULL(ctx, ptr)                                                  \
  do {                                                                         \
    (ctx)->assertions_executed++;                                              \
    if ((ptr) != NULL) {                                                       \
      test_log(ctx, "ASSERT_NULL FAILED: %s:%d: %s not NULL\n", __FILE__,      \
               __LINE__, #ptr);                                                \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_NULL(ctx, ptr)                                                  \
  do {                                                                         \
    (ctx)->expects_executed++;                                                 \
    if ((ptr) != NULL) {                                                       \
      test_log(ctx, "EXPECT_NULL FAILED: %s:%d: %s not NULL\n", __FILE__,      \
               __LINE__, #ptr);                                                \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_NOT_NULL(ctx, ptr)                                              \
  do {                                                                         \
    (ctx)->assertions_executed++;                                              \
    if ((ptr) == NULL) {                                                       \
      test_log(ctx, "ASSERT_NOT_NULL FAILED: %s:%d: %s is NULL\n", __FILE__,   \
               __LINE__, #ptr);                                                \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_NOT_NULL(ctx, ptr)                                              \
  do {                                                                         \
    (ctx)->expects_executed++;                                                 \
    if ((ptr) == NULL) {                                                       \
      test_log(ctx, "EXPECT_NOT_NULL FAILED: %s:%d: %s is NULL\n", __FILE__,   \
               __LINE__, #ptr);                                                \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_MEMEQ(ctx, a, b, size)                                          \
  do {                                                                         \
    const void *_a = (a), *_b = (b);                                           \
    size_t _n = (size);                                                        \
    (ctx)->assertions_executed++;                                              \
    if (memcmp(_a, _b, _n) != 0) {                                             \
      test_log(ctx, "ASSERT_MEMEQ FAILED: %s:%d (size=%zu)\n", __FILE__,       \
               __LINE__, _n);                                                  \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_MEMEQ(ctx, a, b, size)                                          \
  do {                                                                         \
    const void *_a = (a), *_b = (b);                                           \
    size_t _n = (size);                                                        \
    (ctx)->expects_executed++;                                                 \
    if (memcmp(_a, _b, _n) != 0) {                                             \
      test_log(ctx, "EXPECT_MEMEQ FAILED: %s:%d (size=%zu)\n", __FILE__,       \
               __LINE__, _n);                                                  \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_IN_RANGE(ctx, val, lo, hi)                                      \
  do {                                                                         \
    long long _v = (val), _lo = (lo), _hi = (hi);                              \
    (ctx)->assertions_executed++;                                              \
    if (_v < _lo || _v > _hi) {                                                \
      test_log(ctx,                                                            \
               "ASSERT_IN_RANGE FAILED: %s:%d: %s=%lld not in [%lld,%lld]\n",  \
               __FILE__, __LINE__, #val, _v, _lo, _hi);                        \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_IN_RANGE(ctx, val, lo, hi)                                      \
  do {                                                                         \
    long long _v = (val), _lo = (lo), _hi = (hi);                              \
    (ctx)->expects_executed++;                                                 \
    if (_v < _lo || _v > _hi) {                                                \
      test_log(ctx,                                                            \
               "EXPECT_IN_RANGE FAILED: %s:%d: %s=%lld not in [%lld,%lld]\n",  \
               __FILE__, __LINE__, #val, _v, _lo, _hi);                        \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_HAS_FLAG(ctx, val, mask)                                        \
  do {                                                                         \
    unsigned long _v = (val), _m = (mask);                                     \
    (ctx)->assertions_executed++;                                              \
    if (((_v) & (_m)) != (_m)) {                                               \
      test_log(ctx, "ASSERT_HAS_FLAG FAILED: %s:%d: (%s & %s)==0x%lx\n",       \
               __FILE__, __LINE__, #val, #mask, (unsigned long)(_v & _m));     \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_HAS_FLAG(ctx, val, mask)                                        \
  do {                                                                         \
    unsigned long _v = (val), _m = (mask);                                     \
    (ctx)->expects_executed++;                                                 \
    if (((_v) & (_m)) != (_m)) {                                               \
      test_log(ctx, "EXPECT_HAS_FLAG FAILED: %s:%d: (%s & %s)==0x%lx\n",       \
               __FILE__, __LINE__, #val, #mask, (unsigned long)(_v & _m));     \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_OK(ctx, expr)                                                   \
  do {                                                                         \
    int _r = (expr);                                                           \
    (ctx)->assertions_executed++;                                              \
    if (_r != 0) {                                                             \
      test_log(ctx, "ASSERT_OK FAILED: %s:%d: %s returned %d (errno=%d)\n",    \
               __FILE__, __LINE__, #expr, _r, errno);                          \
      (ctx)->status = TEST_FAIL;                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT_OK(ctx, expr)                                                   \
  do {                                                                         \
    int _r = (expr);                                                           \
    (ctx)->expects_executed++;                                                 \
    if (_r != 0) {                                                             \
      test_log(ctx, "EXPECT_OK FAILED: %s:%d: %s returned %d (errno=%d)\n",    \
               __FILE__, __LINE__, #expr, _r, errno);                          \
      (ctx)->expects_failed++;                                                 \
      (ctx)->status = TEST_FAIL;                                               \
    }                                                                          \
  } while (0)

#define ASSERT_FAIL(ctx, msg, ...)                                             \
  do {                                                                         \
    test_log(ctx, "ASSERT_FAIL: %s:%d: " msg "\n", __FILE__, __LINE__,         \
             ##__VA_ARGS__);                                                   \
    (ctx)->status = TEST_FAIL;                                                 \
    return;                                                                    \
  } while (0)

#define EXPECT_FAIL(ctx, msg, ...)                                             \
  do {                                                                         \
    test_log(ctx, "EXPECT_FAIL: %s:%d: " msg "\n", __FILE__, __LINE__,         \
             ##__VA_ARGS__);                                                   \
    (ctx)->expects_executed++;                                                 \
    (ctx)->expects_failed++;                                                   \
    (ctx)->status = TEST_FAIL;                                                 \
  } while (0)

#endif
