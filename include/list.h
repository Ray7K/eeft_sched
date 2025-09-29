#ifndef LIST_H
#define LIST_H

#include <stdbool.h>
#include <stddef.h>

struct list_head {
  struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) {&(name), &(name)}
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
static inline void INIT_LIST_HEAD(struct list_head *list) {
  list->next = list;
  list->prev = list;
}

#define list_entry(ptr, type, member)                                          \
  ((type *)((char *)(ptr) - offsetof(type, member)))

static inline void __list_add(struct list_head *new_entry,
                              struct list_head *prev, struct list_head *next) {
  next->prev = new_entry;
  new_entry->next = next;
  new_entry->prev = prev;
  prev->next = new_entry;
}

static inline void list_add(struct list_head *new_entry,
                            struct list_head *head) {
  __list_add(new_entry, head, head->next);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
  next->prev = prev;
  prev->next = next;
}

static inline void list_del(struct list_head *entry) {
  __list_del(entry->prev, entry->next);
  entry->next = NULL;
  entry->prev = NULL;
}

#define list_first_entry(ptr, type, member)                                    \
  ((ptr)->next == (ptr) ? NULL : list_entry((ptr)->next, type, member))

#define list_for_each_entry_safe(pos, n, head, member)                         \
  for (pos = list_entry((head)->next, __typeof__(*pos), member),               \
      n = list_entry(pos->member.next, __typeof__(*pos), member);              \
       &pos->member != (head);                                                 \
       pos = n, n = list_entry(n->member.next, __typeof__(*pos), member))

static inline bool list_empty(const struct list_head *head) {
  return head->next == head;
}

static inline void list_splice_init(struct list_head *list,
                                    struct list_head *head) {
  if (!list_empty(list)) {
    struct list_head *first = list->next;
    struct list_head *last = list->prev;
    struct list_head *at = head->next;

    first->prev = head;
    head->next = first;

    last->next = at;
    at->prev = last;

    INIT_LIST_HEAD(list);
  }
}

#endif
