#include "tests/test_assert.h"
#include "tests/test_core.h"

#include "lib/list.h"

#include <stdlib.h>

struct test_node {
  int val;
  struct list_head list;
};

static int list_tests_init(test_ctx *ctx) {
  (void)ctx;
  return 0;
}

static void list_tests_exit(test_ctx *ctx) { (void)ctx; }

static void test_init_and_empty(test_ctx *ctx) {
  LIST_HEAD(head);
  EXPECT(ctx, list_empty(&head));
  EXPECT_EQ(ctx, head.next, &head);
  EXPECT_EQ(ctx, head.prev, &head);
}

static void test_add_single_and_delete(test_ctx *ctx) {
  LIST_HEAD(head);
  struct test_node node = {.val = 1};
  INIT_LIST_HEAD(&node.list);

  list_add(&node.list, &head);
  EXPECT_FALSE(ctx, list_empty(&head));
  EXPECT_EQ(ctx, head.next, &node.list);
  EXPECT_EQ(ctx, head.prev, &node.list);
  EXPECT_EQ(ctx, node.list.next, &head);
  EXPECT_EQ(ctx, node.list.prev, &head);

  list_del(&node.list);
  EXPECT_TRUE(ctx, list_empty(&head));
  EXPECT_EQ(ctx, node.list.next, NULL);
  EXPECT_EQ(ctx, node.list.prev, NULL);
}

static void test_add_tail_basic(test_ctx *ctx) {
  LIST_HEAD(head);

  struct test_node a = {.val = 1};
  struct test_node b = {.val = 2};
  struct test_node c = {.val = 3};

  INIT_LIST_HEAD(&a.list);
  INIT_LIST_HEAD(&b.list);
  INIT_LIST_HEAD(&c.list);

  list_add_tail(&a.list, &head);
  list_add_tail(&b.list, &head);
  list_add_tail(&c.list, &head);

  int expected[] = {1, 2, 3};
  int idx = 0;
  struct test_node *pos;

  list_for_each_entry(pos, &head, list) {
    EXPECT_EQ(ctx, pos->val, expected[idx]);
    idx++;
  }

  EXPECT_EQ(ctx, idx, 3);
}

static void test_add_head_and_tail_interleaved(test_ctx *ctx) {
  LIST_HEAD(head);

  struct test_node a = {.val = 1};
  struct test_node b = {.val = 2};
  struct test_node c = {.val = 3};

  list_add(&a.list, &head);      // head: 1
  list_add_tail(&b.list, &head); // tail: 1 → 2
  list_add(&c.list, &head);      // head: 3 → 1 → 2

  int expected[] = {3, 1, 2};
  int idx = 0;
  struct test_node *pos;

  list_for_each_entry(pos, &head, list) {
    EXPECT_EQ(ctx, pos->val, expected[idx]);
    idx++;
  }

  EXPECT_EQ(ctx, idx, 3);
}

static void test_add_multiple_and_iteration(test_ctx *ctx) {
  LIST_HEAD(head);
  struct test_node a = {.val = 1}, b = {.val = 2}, c = {.val = 3};

  list_add(&a.list, &head);
  list_add(&b.list, &head);
  list_add(&c.list, &head);

  struct test_node *pos;
  int expected_vals[] = {3, 2, 1};
  int idx = 0;
  list_for_each_entry(pos, &head, list) {
    EXPECT_EQ(ctx, pos->val, expected_vals[idx]);
    idx++;
  }
  EXPECT_EQ(ctx, idx, 3);
}

static void test_iteration_empty(test_ctx *ctx) {
  LIST_HEAD(head);
  struct test_node *pos;
  int count = 0;
  list_for_each_entry(pos, &head, list) { count++; }
  EXPECT_EQ(ctx, count, 0);
}

static void test_delete_first_and_last(test_ctx *ctx) {
  LIST_HEAD(head);
  struct test_node a = {.val = 1}, b = {.val = 2}, c = {.val = 3};

  list_add(&a.list, &head);
  list_add(&b.list, &head);
  list_add(&c.list, &head);

  struct test_node *first = list_first_entry(&head, struct test_node, list);
  struct test_node *last = list_last_entry(&head, struct test_node, list);

  EXPECT_EQ(ctx, first, &c);
  EXPECT_EQ(ctx, last, &a);

  list_del(&c.list);
  EXPECT_FALSE(ctx, list_empty(&head));
  EXPECT_EQ(ctx, list_first_entry(&head, struct test_node, list), &b);

  list_del(&a.list);
  EXPECT_FALSE(ctx, list_empty(&head));
  EXPECT_EQ(ctx, list_last_entry(&head, struct test_node, list), &b);

  list_del(&b.list);
  EXPECT_TRUE(ctx, list_empty(&head));
}

static void test_splice_empty_src(test_ctx *ctx) {
  LIST_HEAD(dest);
  LIST_HEAD(src);

  struct test_node n = {.val = 1};
  list_add(&n.list, &dest);

  list_splice_init(&src, &dest); // src empty, no change

  EXPECT_FALSE(ctx, list_empty(&dest));
  EXPECT_EQ(ctx, list_first_entry(&dest, struct test_node, list), &n);
}

static void test_splice_into_empty_dest(test_ctx *ctx) {
  LIST_HEAD(dest);
  LIST_HEAD(src);
  struct test_node a = {.val = 1}, b = {.val = 2};

  list_add(&a.list, &src);
  list_add(&b.list, &src);

  list_splice_init(&src, &dest);
  EXPECT_TRUE(ctx, list_empty(&src));
  EXPECT_FALSE(ctx, list_empty(&dest));

  int expected_vals[] = {2, 1};
  struct test_node *pos;
  int idx = 0;
  list_for_each_entry(pos, &dest, list) {
    EXPECT_EQ(ctx, pos->val, expected_vals[idx]);
    idx++;
  }
  EXPECT_EQ(ctx, idx, 2);
}

static void test_splice_various_sizes(test_ctx *ctx) {
  LIST_HEAD(list1);
  LIST_HEAD(list2);

  struct test_node a = {.val = 1}, b = {.val = 2}, c = {.val = 3};
  struct test_node x = {.val = 10}, y = {.val = 20};

  list_add(&a.list, &list1);
  list_add(&b.list, &list1);
  list_add(&c.list, &list1);

  list_add(&x.list, &list2);
  list_add(&y.list, &list2);

  list_splice_init(&list1, &list2);
  EXPECT_TRUE(ctx, list_empty(&list1));

  int expected_vals[] = {3, 2, 1, 20, 10};
  struct test_node *pos;
  int idx = 0;
  list_for_each_entry(pos, &list2, list) {
    EXPECT_EQ(ctx, pos->val, expected_vals[idx]);
    idx++;
  }
  EXPECT_EQ(ctx, idx, 5);
}

static test_case list_cases[] = {
    TEST_CASE(test_init_and_empty),
    TEST_CASE(test_add_single_and_delete),
    TEST_CASE(test_add_multiple_and_iteration),
    TEST_CASE(test_add_tail_basic),
    TEST_CASE(test_add_head_and_tail_interleaved),
    TEST_CASE(test_iteration_empty),
    TEST_CASE(test_delete_first_and_last),
    TEST_CASE(test_splice_empty_src),
    TEST_CASE(test_splice_into_empty_dest),
    TEST_CASE(test_splice_various_sizes),
    {NULL, NULL},
};

test_suite list_suite = {
    .name = "list_suite",
    .init = list_tests_init,
    .exit = list_tests_exit,
    .cases = list_cases,
};

REGISTER_SUITE(list_suite);
