#ifndef TEST_LOG_H
#define TEST_LOG_H

#include "test_core.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static inline void test_log(test_ctx *ctx, const char *fmt, ...) {
  if (!ctx || !fmt)
    return;

  va_list args;
  va_start(args, fmt);

  int written = vsnprintf(ctx->log + ctx->log_offset,
                          ctx->log_size - ctx->log_offset, fmt, args);

  va_end(args);

  if (written < 0)
    return;

  if ((size_t)written >= ctx->log_size - ctx->log_offset)
    ctx->log_offset = ctx->log_size - 1;
  else
    ctx->log_offset += (size_t)written;

  ctx->log[ctx->log_size - 1] = '\0';

  if (ctx->suite && ctx->suite->output_stream)
    fprintf(ctx->suite->output_stream, "%s",
            ctx->log + ctx->log_offset - written);
}

static inline void test_log_reset(test_ctx *ctx) {
  if (!ctx)
    return;
  ctx->log_offset = 0;
  ctx->log[0] = '\0';
}

#endif
