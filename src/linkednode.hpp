// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <concepts>

namespace makebelieve {

/// @class LinkedNode
/// A link in an intrusive, circular, doubly-linked list of @a T. A @a T joins
/// a list by deriving from `LinkedNode<T>`, so linking and unlinking never
/// allocate and cannot fail. A list is kept through one more `LinkedNode<T>`
/// that stands as its head: every link is to a real node, so neither end needs
/// a special case, and an empty list is the head linked to itself.
///
/// A node starts out linked to only itself, and `remove` leaves it so.
/// @pre A node is alone when it is destroyed.
template <class T>
class LinkedNode {
 public:
  LinkedNode* prev = this;
  LinkedNode* next = this;

  LinkedNode() = default;

  LinkedNode(const LinkedNode&) = delete;
  LinkedNode& operator=(const LinkedNode&) = delete;
  LinkedNode(LinkedNode&&) = delete;
  LinkedNode& operator=(LinkedNode&&) = delete;

  ~LinkedNode() { assert(alone() && "LinkedNode destroyed while linked"); }

  /// Whether the node is linked to only itself - as is the head of an empty
  /// list, or a node in no list.
  [[nodiscard]] bool alone() const noexcept { return next == this; }

  /// This node as a @a T. Valid only if `T` inherits `LinkedNode<T>`.
  [[nodiscard]] T* get() noexcept {
    static_assert(std::derived_from<T, LinkedNode>);
    return static_cast<T*>(this);
  }

  /// @copydoc get()
  [[nodiscard]] const T* get() const noexcept {
    static_assert(std::derived_from<T, LinkedNode>);
    return static_cast<const T*>(this);
  }

  /// Links @a node into @a position's list, just before @a position. Given a
  /// list's head, that appends @a node to the list.
  /// @pre @a node is alone.
  static void insert_before(LinkedNode* position, LinkedNode* node) noexcept {
    assert(node->alone() && "LinkedNode already linked");
    node->prev = position->prev;
    node->next = position;
    position->prev->next = node;
    position->prev = node;
  }

  /// Unlinks @a node from its list, leaving it alone.
  static void remove(LinkedNode* node) noexcept {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node;
    node->next = node;
  }
};

}  // namespace makebelieve
