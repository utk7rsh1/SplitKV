#pragma once

#include <cstddef>
#include <functional>
#include <stack>
#include <utility>

namespace splitkv {

template <typename Key, typename Value, typename Compare = std::less<Key>>
class AVLTree {
 public:
  //  Node
  struct Node {
    Key key;
    Value value;
    Node* left;
    Node* right;
    int height;

    Node(const Key& k, const Value& v)
        : key(k), value(v), left(nullptr), right(nullptr), height(1) {}
  };
  //  Iterator  (in-order, using an explicit stack)
  class Iterator {
   public:
    Iterator() = default;

    // Build an iterator that starts at the leftmost node of `root`.
    explicit Iterator(Node* root) { PushLeft(root); }

    bool operator!=(const Iterator& other) const {
      // Two iterators are equal iff both stacks are empty (end) or
      // their top nodes are the same.
      if (stack_.empty() && other.stack_.empty()) return false;
      if (stack_.empty() != other.stack_.empty()) return true;
      return stack_.top() != other.stack_.top();
    }

    bool operator==(const Iterator& other) const { return !(*this != other); }

    Iterator& operator++() {
      if (stack_.empty()) return *this;
      Node* node = stack_.top();
      stack_.pop();
      PushLeft(node->right);
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    std::pair<const Key&, const Value&> operator*() const {
      Node* node = stack_.top();
      return {node->key, node->value};
    }

    const Key& get_key() const { return stack_.top()->key; }
    const Value& get_value() const { return stack_.top()->value; }

   private:
    void PushLeft(Node* node) {
      while (node) {
        stack_.push(node);
        node = node->left;
      }
    }

    std::stack<Node*> stack_;
  };
  //  Constructors / destructor
  AVLTree() : root_(nullptr), size_(0) {}

  ~AVLTree() { DestroyTree(root_); }

  // Non-copyable, movable.
  AVLTree(const AVLTree&) = delete;
  AVLTree& operator=(const AVLTree&) = delete;

  AVLTree(AVLTree&& other) noexcept
      : root_(other.root_), size_(other.size_), comp_(std::move(other.comp_)) {
    other.root_ = nullptr;
    other.size_ = 0;
  }

  AVLTree& operator=(AVLTree&& other) noexcept {
    if (this != &other) {
      DestroyTree(root_);
      root_ = other.root_;
      size_ = other.size_;
      comp_ = std::move(other.comp_);
      other.root_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }
  //  Public API

  /// Insert or update.
  void Insert(const Key& key, const Value& value) {
    bool inserted = false;
    root_ = Insert(root_, key, value, inserted);
    if (inserted) ++size_;
  }

  /// Return true and write *value if found.
  bool Find(const Key& key, Value* value) const {
    Node* node = root_;
    while (node) {
      if (comp_(key, node->key)) {
        node = node->left;
      } else if (comp_(node->key, key)) {
        node = node->right;
      } else {
        if (value) *value = node->value;
        return true;
      }
    }
    return false;
  }

  /// Return true if key existed and was removed.
  bool Remove(const Key& key) {
    bool found = false;
    root_ = Remove(root_, key, found);
    if (found) --size_;
    return found;
  }

  size_t Size() const { return size_; }
  bool Empty() const { return size_ == 0; }

  void Clear() {
    DestroyTree(root_);
    root_ = nullptr;
    size_ = 0;
  }

  Iterator begin() const { return Iterator(root_); }
  Iterator end() const { return Iterator(); }

 private:
  //  Helpers

  static int Height(Node* node) { return node ? node->height : 0; }

  static int BalanceFactor(Node* node) {
    return node ? Height(node->left) - Height(node->right) : 0;
  }

  static void UpdateHeight(Node* node) {
    if (node) {
      int lh = Height(node->left);
      int rh = Height(node->right);
      node->height = 1 + (lh > rh ? lh : rh);
    }
  }

  // Right rotation (LL case).
  //       y            x
  //      / \          / \
  //     x   C  =>   A   y
  //    / \              / \
  //   A   B            B   C
  static Node* RotateRight(Node* y) {
    Node* x = y->left;
    Node* B = x->right;
    x->right = y;
    y->left = B;
    UpdateHeight(y);
    UpdateHeight(x);
    return x;
  }

  // Left rotation (RR case).
  //     x              y
  //    / \            /  \
  //   A   y   =>    x    C
  //      / \       / \
  //     B   C     A   B
  static Node* RotateLeft(Node* x) {
    Node* y = x->right;
    Node* B = y->left;
    y->left = x;
    x->right = B;
    UpdateHeight(x);
    UpdateHeight(y);
    return y;
  }

  /// Balance `node` after an insertion or deletion.
  Node* Balance(Node* node) {
    UpdateHeight(node);
    int bf = BalanceFactor(node);

    // Left-heavy
    if (bf > 1) {
      if (BalanceFactor(node->left) < 0) {
        // LR case — left-rotate left child first.
        node->left = RotateLeft(node->left);
      }
      return RotateRight(node);  // LL case (or LR after fix).
    }

    // Right-heavy
    if (bf < -1) {
      if (BalanceFactor(node->right) > 0) {
        // RL case — right-rotate right child first.
        node->right = RotateRight(node->right);
      }
      return RotateLeft(node);  // RR case (or RL after fix).
    }

    return node;
  }

  /// Recursive insert. Sets `inserted` to true if a new node was created.
  Node* Insert(Node* node, const Key& key, const Value& value,
               bool& inserted) {
    if (!node) {
      inserted = true;
      return new Node(key, value);
    }

    if (comp_(key, node->key)) {
      node->left = Insert(node->left, key, value, inserted);
    } else if (comp_(node->key, key)) {
      node->right = Insert(node->right, key, value, inserted);
    } else {
      // Duplicate key — update value in place.
      node->value = value;
      return node;
    }

    return Balance(node);
  }

  static Node* FindMin(Node* node) {
    while (node && node->left) node = node->left;
    return node;
  }

  /// Recursive remove. Sets `found` to true if the key existed.
  Node* Remove(Node* node, const Key& key, bool& found) {
    if (!node) return nullptr;

    if (comp_(key, node->key)) {
      node->left = Remove(node->left, key, found);
    } else if (comp_(node->key, key)) {
      node->right = Remove(node->right, key, found);
    } else {
      // Found the node to delete.
      found = true;

      if (!node->left || !node->right) {
        // 0 or 1 child.
        Node* child = node->left ? node->left : node->right;
        delete node;
        return child;  // may be nullptr
      }

      // 2 children — replace with in-order successor.
      Node* successor = FindMin(node->right);
      node->key = successor->key;
      node->value = successor->value;
      node->right = Remove(node->right, successor->key, found);
      // `found` was already set to true; the recursive call will also set it,
      // which is fine.
    }

    return Balance(node);
  }

  void DestroyTree(Node* node) {
    if (!node) return;
    DestroyTree(node->left);
    DestroyTree(node->right);
    delete node;
  }
  //  Members
  Node* root_;
  size_t size_;
  Compare comp_;
};

}  // namespace splitkv
