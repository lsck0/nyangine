/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

typedef struct TestNode TestNode;
struct TestNode {
  s32       value;
  TestNode* prev;
  TestNode* next;
};

typedef struct {
  TestNode* head;
  TestNode* tail;
} TestDLL;

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_dll");

  // TEST: nya_dll_node_push_back on empty list
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;

    nya_dll_node_push_back(&dll, n1);
    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n1);
    nya_assert(n1->prev == nullptr);
    nya_assert(n1->next == nullptr);
  }

  // TEST: nya_dll_node_push_back multiple nodes
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);
    nya_dll_node_push_back(&dll, n3);

    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n3);
    nya_assert(n1->prev == nullptr);
    nya_assert(n1->next == n2);
    nya_assert(n2->prev == n1);
    nya_assert(n2->next == n3);
    nya_assert(n3->prev == n2);
    nya_assert(n3->next == nullptr);
  }

  // TEST: nya_dll_node_push_front on empty list
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;

    nya_dll_node_push_front(&dll, n1);
    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n1);
    nya_assert(n1->prev == nullptr);
    nya_assert(n1->next == nullptr);
  }

  // TEST: nya_dll_node_push_front multiple nodes
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_front(&dll, n1);
    nya_dll_node_push_front(&dll, n2);
    nya_dll_node_push_front(&dll, n3);

    // Order should be: n3 -> n2 -> n1
    nya_assert(dll.head == n3);
    nya_assert(dll.tail == n1);
    nya_assert(n3->prev == nullptr);
    nya_assert(n3->next == n2);
    nya_assert(n2->prev == n3);
    nya_assert(n2->next == n1);
    nya_assert(n1->prev == n2);
    nya_assert(n1->next == nullptr);
  }

  // TEST: nya_dll_node_pop_back
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);
    nya_dll_node_push_back(&dll, n3);

    TestNode* popped = nya_dll_node_pop_back(&dll);
    nya_assert(popped == n3);
    nya_assert(popped->value == 3);
    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n2);
    nya_assert(n2->next == nullptr);

    popped = nya_dll_node_pop_back(&dll);
    nya_assert(popped == n2);
    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n1);

    popped = nya_dll_node_pop_back(&dll);
    nya_assert(popped == n1);
    nya_assert(dll.head == nullptr);
    nya_assert(dll.tail == nullptr);
  }

  // TEST: nya_dll_node_pop_front
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);
    nya_dll_node_push_back(&dll, n3);

    TestNode* popped = nya_dll_node_pop_front(&dll);
    nya_assert(popped == n1);
    nya_assert(popped->value == 1);
    nya_assert(dll.head == n2);
    nya_assert(dll.tail == n3);
    nya_assert(n2->prev == nullptr);

    popped = nya_dll_node_pop_front(&dll);
    nya_assert(popped == n2);
    nya_assert(dll.head == n3);
    nya_assert(dll.tail == n3);

    popped = nya_dll_node_pop_front(&dll);
    nya_assert(popped == n3);
    nya_assert(dll.head == nullptr);
    nya_assert(dll.tail == nullptr);
  }

  // TEST: nya_dll_node_link in the middle
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n3);

    // Insert n2 between n1 and n3
    nya_dll_node_link(&dll, n1, n2, n3);

    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n3);
    nya_assert(n1->next == n2);
    nya_assert(n2->prev == n1);
    nya_assert(n2->next == n3);
    nya_assert(n3->prev == n2);
  }

  // TEST: nya_dll_node_unlink from middle
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);
    nya_dll_node_push_back(&dll, n3);

    nya_dll_node_unlink(&dll, n2);

    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n3);
    nya_assert(n1->next == n3);
    nya_assert(n3->prev == n1);
    nya_assert(n2->prev == nullptr);
    nya_assert(n2->next == nullptr);
  }

  // TEST: nya_dll_node_unlink from head
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);

    nya_dll_node_unlink(&dll, n1);

    nya_assert(dll.head == n2);
    nya_assert(dll.tail == n2);
    nya_assert(n2->prev == nullptr);
    nya_assert(n1->prev == nullptr);
    nya_assert(n1->next == nullptr);
  }

  // TEST: nya_dll_node_unlink from tail
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);

    nya_dll_node_unlink(&dll, n2);

    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n1);
    nya_assert(n1->next == nullptr);
    nya_assert(n2->prev == nullptr);
    nya_assert(n2->next == nullptr);
  }

  // TEST: nya_dll_node_unlink single node
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_unlink(&dll, n1);

    nya_assert(dll.head == nullptr);
    nya_assert(dll.tail == nullptr);
    nya_assert(n1->prev == nullptr);
    nya_assert(n1->next == nullptr);
  }

  // TEST: nya_dll_foreach
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);
    nya_dll_node_push_back(&dll, n3);

    s32 sum   = 0;
    s32 count = 0;
    nya_dll_foreach (&dll, node) {
      sum += node->value;
      count++;
    }
    nya_assert(sum == 6);
    nya_assert(count == 3);
  }

  // TEST: nya_dll_foreach on empty list
  {
    TestDLL dll   = { 0 };
    s32     count = 0;
    nya_dll_foreach (&dll, node) { count++; }
    nya_assert(count == 0);
  }

  // TEST: nya_dll_foreach_reverse
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);
    nya_dll_node_push_back(&dll, n3);

    s32 values[3];
    s32 idx = 0;
    nya_dll_foreach_reverse(&dll, node) {
      values[idx++] = node->value;
    }

    nya_assert(values[0] == 3);
    nya_assert(values[1] == 2);
    nya_assert(values[2] == 1);
  }

  // TEST: nya_dll_foreach_reverse on empty list
  {
    TestDLL dll   = { 0 };
    s32     count = 0;
    nya_dll_foreach_reverse(&dll, node) {
      count++;
    }
    nya_assert(count == 0);
  }

  // TEST: mixed push_front and push_back
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n3  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n4  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;
    n3->value     = 3;
    n4->value     = 4;

    nya_dll_node_push_back(&dll, n2);  // [2]
    nya_dll_node_push_front(&dll, n1); // [1, 2]
    nya_dll_node_push_back(&dll, n3);  // [1, 2, 3]
    nya_dll_node_push_front(&dll, n4); // [4, 1, 2, 3]

    nya_assert(dll.head == n4);
    nya_assert(dll.tail == n3);

    s32 expected[] = { 4, 1, 2, 3 };
    s32 idx        = 0;
    nya_dll_foreach (&dll, node) {
      nya_assert(node->value == expected[idx]);
      idx++;
    }
    nya_assert(idx == 4);
  }

  // TEST: re-linking after unlink
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;

    nya_dll_node_push_back(&dll, n1);
    nya_dll_node_push_back(&dll, n2);

    // Unlink n1 and push it to the back
    nya_dll_node_unlink(&dll, n1);
    nya_dll_node_push_back(&dll, n1);

    nya_assert(dll.head == n2);
    nya_assert(dll.tail == n1);
    nya_assert(n2->next == n1);
    nya_assert(n1->prev == n2);
  }

  // TEST: large list operations
  {
    TestDLL    dll   = { 0 };
    TestNode** nodes = (TestNode**)nya_arena_alloc(arena, 100 * sizeof(TestNode*));
    for (s32 i = 0; i < 100; ++i) {
      nodes[i]        = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
      nodes[i]->value = i;
      nya_dll_node_push_back(&dll, nodes[i]);
    }

    // Verify forward traversal
    s32 expected_val = 0;
    nya_dll_foreach (&dll, node) {
      nya_assert(node->value == expected_val);
      expected_val++;
    }
    nya_assert(expected_val == 100);

    // Verify reverse traversal
    expected_val = 99;
    nya_dll_foreach_reverse(&dll, node) {
      nya_assert(node->value == expected_val);
      expected_val--;
    }
    nya_assert(expected_val == -1);

    // Remove every other node
    for (s32 i = 0; i < 100; i += 2) { nya_dll_node_unlink(&dll, nodes[i]); }

    // Verify remaining
    s32 count = 0;
    nya_dll_foreach (&dll, node) {
      nya_assert(node->value % 2 == 1); // only odd values remain
      count++;
    }
    nya_assert(count == 50);
  }

  // TEST: pop from single element list
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 42;

    nya_dll_node_push_back(&dll, n1);
    TestNode* popped = nya_dll_node_pop_front(&dll);

    nya_assert(popped == n1);
    nya_assert(dll.head == nullptr);
    nya_assert(dll.tail == nullptr);
  }

  // TEST: nya_dll_node_link at boundaries
  {
    TestDLL   dll = { 0 };
    TestNode* n1  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    TestNode* n2  = (TestNode*)nya_arena_alloc(arena, sizeof(TestNode));
    n1->value     = 1;
    n2->value     = 2;

    // Link n1 as head (prev = nullptr)
    nya_dll_node_link(&dll, nullptr, n1, nullptr);
    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n1);

    // Link n2 at the end
    nya_dll_node_link(&dll, n1, n2, nullptr);
    nya_assert(dll.head == n1);
    nya_assert(dll.tail == n2);
    nya_assert(n1->next == n2);
    nya_assert(n2->prev == n1);
  }

  nya_arena_destroy(arena);

  return 0;
}
