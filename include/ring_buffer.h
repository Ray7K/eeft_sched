#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  _Atomic uint64_t head;
  _Atomic uint64_t tail;
  _Atomic uint64_t *seq;
  uint64_t buf_size;
  uint64_t buf_elem_size;
  void *buffer;
} ring_buffer_t;

static inline void ring_buffer_init(ring_buffer_t *rb, uint64_t size,
                                    void *buffer, _Atomic uint64_t *seq,
                                    uint64_t elem_size) {
  rb->buf_size = size;
  rb->buffer = buffer;
  rb->buf_elem_size = elem_size;
  rb->seq = seq;

  for (uint64_t i = 0; i < size; i++) {
    atomic_init(&rb->seq[i], i);
  }

  atomic_store(&rb->head, 0);
  atomic_store(&rb->tail, 0);
}

static inline int ring_buffer_try_enqueue(ring_buffer_t *rb, void *elem) {
  uint64_t tail = atomic_load(&rb->tail);

  if (atomic_load_explicit(&rb->seq[tail % rb->buf_size],
                           memory_order_acquire) != tail)
    return -1;

  if (!atomic_compare_exchange_strong(&rb->tail, &tail, tail + 1)) {
    return -1;
  }

  memcpy((char *)rb->buffer + (tail % rb->buf_size) * rb->buf_elem_size, elem,
         rb->buf_elem_size);

  atomic_store_explicit(&rb->seq[tail % rb->buf_size], tail + 1,
                        memory_order_release);
  return 0;
}

static inline int ring_buffer_enqueue(ring_buffer_t *rb, void *elem) {
  uint64_t tail = atomic_fetch_add(&rb->tail, 1);

  while (atomic_load_explicit(&rb->seq[tail % rb->buf_size],
                              memory_order_acquire) != tail)
    ;

  memcpy((char *)rb->buffer + (tail % rb->buf_size) * rb->buf_elem_size, elem,
         rb->buf_elem_size);

  atomic_store_explicit(&rb->seq[tail % rb->buf_size], tail + 1,
                        memory_order_release);
  return 0;
}

static inline int ring_buffer_try_dequeue(ring_buffer_t *rb, void *elem) {
  uint64_t head = atomic_load(&rb->head);

  if (atomic_load_explicit(&rb->seq[head % rb->buf_size],
                           memory_order_acquire) != head + 1)
    return -1;

  if (!atomic_compare_exchange_strong(&rb->head, &head, head + 1)) {
    return -1;
  }

  memcpy(elem, (char *)rb->buffer + (head % rb->buf_size) * rb->buf_elem_size,
         rb->buf_elem_size);

  atomic_store_explicit(&rb->seq[head % rb->buf_size], head + rb->buf_size,
                        memory_order_release);
  return 0;
}

static inline int ring_buffer_dequeue(ring_buffer_t *rb, void *elem) {
  uint64_t head = atomic_fetch_add(&rb->head, 1);

  while (atomic_load_explicit(&rb->seq[head % rb->buf_size],
                              memory_order_acquire) != head + 1)
    ;

  memcpy(elem, (char *)rb->buffer + (head % rb->buf_size) * rb->buf_elem_size,
         rb->buf_elem_size);

  atomic_store_explicit(&rb->seq[head % rb->buf_size], head + rb->buf_size,
                        memory_order_release);
  return 0;
}

#define ring_buffer_iter_read_unsafe(rb, elem_ptr)                             \
  for (uint64_t idx = atomic_load(&rb->head),                                  \
                elem_ptr = (char *)rb->buffer +                                \
                           (idx % rb->buf_size * rb->buf_elem_size);           \
       idx < atomic_load(&rb->tail);                                           \
       idx++, elem_ptr = (char *)rb->buffer +                                  \
                         (idx % rb->buf_size * rb->buf_elem_size))

#endif
