#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
class Node;
// Bound borrowed-subtree traversal and reject cycles through render proxies.
// Each caller owns a separate stack; repeated sibling references remain legal.
class RenderTraversalGuard {
public:
  struct Stack { std::array<const Node*, 256> nodes{}; std::size_t size = 0; };
  RenderTraversalGuard(Stack& stack, const Node* node) : m_stack(stack) {
    if (stack.size == stack.nodes.size()
        || std::find(stack.nodes.begin(), stack.nodes.begin() + stack.size, node)
            != stack.nodes.begin() + stack.size) return;
    stack.nodes[stack.size++] = node;
    m_entered = true;
  }
  ~RenderTraversalGuard() { if (m_entered) --m_stack.size; }
  explicit operator bool() const { return m_entered; }
  RenderTraversalGuard(const RenderTraversalGuard&) = delete;
  RenderTraversalGuard& operator=(const RenderTraversalGuard&) = delete;
private:
  Stack& m_stack;
  bool m_entered = false;
};
