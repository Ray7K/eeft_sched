#include "list.h"
#include "tests.h"
#include <stddef.h>

struct my_node {
  int data;
  struct list_head list;
};

static void test_list_init_empty(TestCase *test) {
  LIST_HEAD(head);
  EXPECT(test, list_empty(&head));
  EXPECT(test, head.next == &head);
  EXPECT(test, head.prev == &head);

  INIT_LIST_HEAD(&head);
  EXPECT(test, list_empty(&head));
  EXPECT(test, head.next == &head);
  EXPECT(test, head.prev == &head);
}

static void test_list_add_and_first(TestCase *test) {
  LIST_HEAD(head);
  struct my_node *first = list_first_entry(&head, struct my_node, list);
  ASSERT(test, first == NULL);

  struct my_node node1;
  node1.data = 1;
  list_add(&node1.list, &head);

  ASSERT(test, !list_empty(&head));
  first = list_first_entry(&head, struct my_node, list);
  ASSERT(test, first == &node1);
  EXPECT(test, first->data == 1);

  struct my_node node2;
  node2.data = 2;
  list_add(&node2.list, &head);

  first = list_first_entry(&head, struct my_node, list);
  ASSERT(test, first == &node2);
  EXPECT(test, first->data == 2);
  EXPECT(test, head.next == &node2.list);
  EXPECT(test, node2.list.next == &node1.list);
  EXPECT(test, node1.list.next == &head);
  EXPECT(test, head.prev == &node1.list);
  EXPECT(test, node1.list.prev == &node2.list);
  EXPECT(test, node2.list.prev == &head);
}

// Test case for deleting an element from the list
static void test_list_del(TestCase *test) {
  LIST_HEAD(head);
  struct my_node node1;
  node1.data = 1;
  list_add(&node1.list, &head);
  ASSERT(test, !list_empty(&head));

  list_del(&node1.list);
  ASSERT(test, list_empty(&head));
  EXPECT(test, node1.list.next == NULL);
  EXPECT(test, node1.list.prev == NULL);
}

static void test_list_iteration(TestCase *test) {
  LIST_HEAD(head);
  struct my_node nodes[5];
  for (int i = 0; i < 5; i++) {
    nodes[i].data = i;
    list_add(&nodes[i].list, &head);
  }

  int expected_data = 4;
  struct my_node *pos;
  list_for_each_entry(pos, &head, list) {
    EXPECT(test, pos->data == expected_data);
    expected_data--;
  }
  ASSERT(test, expected_data == -1);
}

static void test_list_iteration_safe(TestCase *test) {
  LIST_HEAD(head);
  struct my_node nodes[5];
  for (int i = 0; i < 5; i++) {
    nodes[i].data = i;
    list_add(&nodes[i].list, &head);
  }

  int count = 0;
  struct my_node *pos, *n;
  list_for_each_entry_safe(pos, n, &head, list) {
    list_del(&pos->list);
    count++;
  }
  ASSERT(test, count == 5);
  ASSERT(test, list_empty(&head));
}

static void test_list_splice(TestCase *test) {
  LIST_HEAD(list1);
  struct my_node nodes1[3];
  for (int i = 0; i < 3; i++) {
    nodes1[i].data = i;
    list_add(&nodes1[i].list, &list1); // list1: 2, 1, 0
  }

  LIST_HEAD(list2);
  struct my_node nodes2[2];
  for (int i = 0; i < 2; i++) {
    nodes2[i].data = i + 3;
    list_add(&nodes2[i].list, &list2); // list2: 4, 3
  }

  list_splice_init(&list1, &list2);

  ASSERT(test, list_empty(&list1));

  int expected_order[] = {2, 1, 0, 4, 3};
  int i = 0;
  struct my_node *pos;
  list_for_each_entry(pos, &list2, list) {
    EXPECT(test, pos->data == expected_order[i]);
    i++;
  }
  ASSERT(test, i == 5);
}

REGISTER_TEST("list_init_empty", test_list_init_empty, NULL, NULL);
REGISTER_TEST("list_add_and_first", test_list_add_and_first, NULL, NULL);
REGISTER_TEST("list_del", test_list_del, NULL, NULL);
REGISTER_TEST("list_iteration", test_list_iteration, NULL, NULL);
REGISTER_TEST("list_iteration_safe", test_list_iteration_safe, NULL, NULL);
REGISTER_TEST("list_splice", test_list_splice, NULL, NULL);
