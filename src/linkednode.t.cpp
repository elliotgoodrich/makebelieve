// SPDX-License-Identifier: MIT
#include "linkednode.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace {

struct Item;
using Node = makebelieve::LinkedNode<Item>;

struct Item : Node {
  int value;
  explicit Item(int value) : value(value) {}
};

// The values from the first node after @a head round to @a head, checking
// each node's `prev` agrees with the walk.
std::vector<int> values(const Node& head) {
  std::vector<int> result;
  const Node* previous = &head;
  for (const Node* node = head.next; node != &head; node = node->next) {
    EXPECT_EQ(node->prev, previous);
    result.push_back(node->get()->value);
    previous = node;
  }
  EXPECT_EQ(head.prev, previous);
  return result;
}

TEST(LinkedNode, StartsAlone) {
  const Node node;
  EXPECT_TRUE(node.alone());
  EXPECT_EQ(node.next, &node);
  EXPECT_EQ(node.prev, &node);
}

TEST(LinkedNode, GetReturnsTheItemTheNodeIsPartOf) {
  Item item(7);
  Node& node = item;
  EXPECT_EQ(node.get(), &item);
  EXPECT_EQ(std::as_const(node).get(), &item);
}

TEST(LinkedNode, InsertingBeforeTheHeadAppends) {
  Node head;
  Item a(1);
  Item b(2);
  Item c(3);
  Node::insert_before(&head, &a);
  Node::insert_before(&head, &b);
  Node::insert_before(&head, &c);
  EXPECT_FALSE(head.alone());
  EXPECT_EQ(values(head), (std::vector{1, 2, 3}));

  Node::remove(&a);
  Node::remove(&b);
  Node::remove(&c);
}

TEST(LinkedNode, InsertingBeforeANodePutsItThere) {
  Node head;
  Item a(1);
  Item c(3);
  Item b(2);
  Node::insert_before(&head, &a);
  Node::insert_before(&head, &c);
  Node::insert_before(&c, &b);
  EXPECT_EQ(values(head), (std::vector{1, 2, 3}));

  Node::remove(&a);
  Node::remove(&b);
  Node::remove(&c);
}

TEST(LinkedNode, RemovingUnlinksFromAnywhereAndLeavesTheNodeAlone) {
  Node head;
  Item a(1);
  Item b(2);
  Item c(3);
  Node::insert_before(&head, &a);
  Node::insert_before(&head, &b);
  Node::insert_before(&head, &c);

  Node::remove(&b);
  EXPECT_TRUE(b.alone());
  EXPECT_EQ(values(head), (std::vector{1, 3}));

  Node::remove(&c);
  EXPECT_EQ(values(head), (std::vector{1}));

  Node::remove(&a);
  EXPECT_TRUE(head.alone());
  EXPECT_EQ(values(head), (std::vector<int>{}));
}

TEST(LinkedNode, ARemovedNodeCanBeInsertedAgain) {
  Node head;
  Item a(1);
  Item b(2);
  Node::insert_before(&head, &a);
  Node::insert_before(&head, &b);
  Node::remove(&a);
  Node::insert_before(&head, &a);
  EXPECT_EQ(values(head), (std::vector{2, 1}));

  Node::remove(&a);
  Node::remove(&b);
}

}  // namespace
