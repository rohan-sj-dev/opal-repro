// B+ Tree with lock coupling, templated on the lock implementation.
//
// Follows Listing 3 of the paper: the default traversal (left column) and the
// function-pointer conversion of the update critical section (right column).
// Nodes are ~256 bytes with 8-byte keys and values, matching Section 5
// "Evaluation setup".
//
// Nodes are never freed, so an optimistic reader can always safely dereference
// a child pointer it read from a node whose version later turns out to be
// stale. That is the usual memory-reclamation assumption for OLC indexes and it
// is what the paper's setup implies too (no delete in any workload of Table 1).
#pragma once
#include "opal.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdio>
#include <vector>

namespace idx {

// Per-thread retry counters; the paper reports reader retries as the mechanism
// behind the opportunistic-read result (Section 5.1).
extern thread_local uint64_t t_read_retries;
extern thread_local uint64_t t_write_retries;

template <class Lock>
class BTree {
 public:
  static constexpr int LK = 15;  // 16 + 15*8 + 15*8 = 256 bytes with an 8-byte lock
  static constexpr int IK = 15;

  struct NodeBase {
    Lock lock;
    uint32_t is_leaf;
    uint32_t count;
  };
  struct alignas(64) Leaf : NodeBase {
    uint64_t keys[LK];
    uint64_t vals[LK];
  };
  struct alignas(64) Inner : NodeBase {
    uint64_t keys[IK];
    NodeBase* child[IK + 1];
  };

  BTree() {
    sentinel_ = new_inner();
    sentinel_->count = 0;
    sentinel_->child[0] = new_leaf();
  }

  // ------------------------------------------------------------------ lookup
  bool lookup(uint64_t key, uint64_t* out) {
  restart:
    NodeBase* parent = sentinel_;
    uint64_t v;
    if (!parent->lock.read_lock(v)) { ++t_read_retries; goto restart; }
    NodeBase* child = descend(parent, key);
    for (;;) {
      uint64_t nv;
      if (!child->lock.read_lock(nv)) {
        parent->lock.read_abort(v);
        ++t_read_retries;
        goto restart;
      }
      if (!parent->lock.read_unlock(v)) {
        child->lock.read_abort(nv);
        ++t_read_retries;
        goto restart;
      }
      if (child->is_leaf) {
        Leaf* l = static_cast<Leaf*>(child);
        bool found = leaf_find(l, key, out);
        if (!l->lock.read_unlock(nv)) { ++t_read_retries; goto restart; }
        return found;
      }
      parent = child;
      v = nv;
      child = descend(parent, key);
    }
  }

  // ------------------------------------------------------------------ update
  // In-place value update. No structural change, so this is the operation Opal
  // routes through the combiner (paper, Section 3.4 "B+ Tree").
  bool update(uint64_t key, uint64_t new_val) {
  restart:
    NodeBase* parent = sentinel_;
    uint64_t v;
    if (!parent->lock.read_lock(v)) { ++t_write_retries; goto restart; }
    NodeBase* child = descend(parent, key);
    for (;;) {
      if (child->is_leaf) {
        if (Lock::kUseFnPtr) {
          UpdateArg a{parent, v, static_cast<Leaf*>(child), key, new_val, false};
          if (!child->lock.execute_op(&update_fn, &a)) {
            ++t_write_retries;
            goto restart;
          }
          return a.found;
        } else {
          child->lock.write_lock();
          if (!parent->lock.read_unlock(v)) {
            child->lock.write_unlock();
            ++t_write_retries;
            goto restart;
          }
          bool found = leaf_update(static_cast<Leaf*>(child), key, new_val);
          child->lock.write_unlock();
          return found;
        }
      }
      uint64_t nv;
      if (!child->lock.read_lock(nv)) {
        parent->lock.read_abort(v);
        ++t_write_retries;
        goto restart;
      }
      if (!parent->lock.read_unlock(v)) {
        child->lock.read_abort(nv);
        ++t_write_retries;
        goto restart;
      }
      parent = child;
      v = nv;
      child = descend(parent, key);
    }
  }

  // ------------------------------------------------------------------ insert
  // Optimistic descent, then a plain (non-batched) write lock on the leaf: Opal
  // deliberately keeps SMOs on the traditional MCS path (Section 3, Section 5.2).
  // If the leaf is full we fall back to a top-down write-coupled descent that
  // holds every ancestor that could split.
  bool insert(uint64_t key, uint64_t val) {
    uint64_t lo, hi;
  restart:
    lo = 0; hi = ~0ull;
    NodeBase* parent = sentinel_;
    uint64_t v;
    if (!parent->lock.read_lock(v)) { ++t_write_retries; goto restart; }
    NodeBase* child = descend_range(parent, key, &lo, &hi);
    for (;;) {
      if (child->is_leaf) {
        Leaf* l = static_cast<Leaf*>(child);
        l->lock.write_lock();
        if (!parent->lock.read_unlock(v)) {
          l->lock.write_unlock();
          ++t_write_retries;
          goto restart;
        }
#ifdef OPAL_DEBUG_RANGE
        if (descend(parent, key) != child) {
          std::fprintf(stderr, "[fastpath] parent no longer points at leaf\n");
          std::fflush(stderr);
          std::abort();
        }
        check_leaf_range(l, key, lo, hi, "fastpath");
#endif
#ifndef OPAL_NO_INSERT_FASTPATH
        if (l->count < LK) {
          bool ins = leaf_insert(l, key, val);
          l->lock.write_unlock();
          return ins;
        }
#endif
        l->lock.write_unlock();
        return insert_smo(key, val);  // leaf is full: structural modification
      }
      uint64_t nv;
      if (!child->lock.read_lock(nv)) {
        parent->lock.read_abort(v);
        ++t_write_retries;
        goto restart;
      }
      if (!parent->lock.read_unlock(v)) {
        child->lock.read_abort(nv);
        ++t_write_retries;
        goto restart;
      }
      parent = child;
      v = nv;
      child = descend_range(parent, key, &lo, &hi);
    }
  }

  // Full structural check: key order, separator ranges, uniform leaf depth.
  // Single-threaded; returns an empty string when the tree is consistent.
  std::string validate() const {
    std::string err;
    int leaf_depth = -1;
    validate(sentinel_->child[0], 0, ~0ull, 0, &leaf_depth, &err);
    return err;
  }

  // Single-threaded consistency check used after a run.
  size_t count_keys() const { return count_keys(sentinel_->child[0]); }
  int height() const {
    int h = 1;
    NodeBase* n = sentinel_->child[0];
    while (!n->is_leaf) { n = static_cast<Inner*>(n)->child[0]; ++h; }
    return h;
  }

 private:
  Inner* sentinel_;  // count == 0 forever; child[0] is the real root

  struct UpdateArg {
    NodeBase* parent;
    uint64_t v;
    Leaf* leaf;
    uint64_t key;
    uint64_t val;
    bool found;
  };

  // Listing 3, lines 32-36: the update critical section as a function the
  // combiner can run on the waiter's behalf.
  static bool update_fn(void* p) {
    UpdateArg* a = static_cast<UpdateArg*>(p);
    if (!a->parent->lock.read_unlock(a->v)) return false;
    a->found = leaf_update(a->leaf, a->key, a->val);
    return true;
  }

  // Same as descend(), but also narrows the [lo,hi) key range the chosen child
  // is responsible for. Debug builds use it to catch a traversal that lands on
  // the wrong leaf.
  static NodeBase* descend_range(NodeBase* n, uint64_t key, uint64_t* lo,
                                 uint64_t* hi) {
    Inner* in = static_cast<Inner*>(n);
    uint32_t c = in->count;
    if (c > IK) c = IK;
    uint32_t i = 0;
    while (i < c && key >= in->keys[i]) ++i;
    if (i > 0) *lo = in->keys[i - 1];
    if (i < c) *hi = in->keys[i];
    return in->child[i];
  }

  static NodeBase* descend(NodeBase* n, uint64_t key) {
    Inner* in = static_cast<Inner*>(n);
    uint32_t c = in->count;
    if (c > IK) c = IK;  // an optimistic reader may observe a torn count
    uint32_t i = 0;
    while (i < c && key >= in->keys[i]) ++i;
    return in->child[i];
  }

  static bool leaf_find(Leaf* l, uint64_t key, uint64_t* out) {
    uint32_t c = l->count;
    for (uint32_t i = 0; i < c && i < LK; ++i)
      if (l->keys[i] == key) { *out = l->vals[i]; return true; }
    return false;
  }

  static bool leaf_update(Leaf* l, uint64_t key, uint64_t val) {
    uint32_t c = l->count;
    for (uint32_t i = 0; i < c && i < LK; ++i)
      if (l->keys[i] == key) { l->vals[i] = val; return true; }
    return false;
  }

  static bool leaf_insert(Leaf* l, uint64_t key, uint64_t val) {
    uint32_t i = 0;
    while (i < l->count && l->keys[i] < key) ++i;
    if (i < l->count && l->keys[i] == key) { l->vals[i] = val; return false; }
    for (uint32_t j = l->count; j > i; --j) {
      l->keys[j] = l->keys[j - 1];
      l->vals[j] = l->vals[j - 1];
    }
    l->keys[i] = key;
    l->vals[i] = val;
    l->count = l->count + 1;
    return true;
  }

  static void inner_insert(Inner* n, uint64_t key, NodeBase* right) {
    uint32_t i = 0;
    while (i < n->count && n->keys[i] < key) ++i;
    for (uint32_t j = n->count; j > i; --j) {
      n->keys[j] = n->keys[j - 1];
      n->child[j + 1] = n->child[j];
    }
    n->keys[i] = key;
    n->child[i + 1] = right;
    n->count = n->count + 1;
  }

  static Leaf* new_leaf() {
    void* p = _aligned_malloc(sizeof(Leaf), 64);
    Leaf* l = new (p) Leaf();
    l->is_leaf = 1;
    l->count = 0;
    return l;
  }
  static Inner* new_inner() {
    void* p = _aligned_malloc(sizeof(Inner), 64);
    Inner* n = new (p) Inner();
    n->is_leaf = 0;
    n->count = 0;
    return n;
  }

#ifdef OPAL_DEBUG_RANGE
  // A leaf reached for `key` must already hold only keys inside the separator
  // range the traversal narrowed to, and `key` must fall inside it.
  static void check_leaf_range(Leaf* l, uint64_t key, uint64_t lo, uint64_t hi,
                               const char* where) {
    if (key < lo || key >= hi) {
      std::fprintf(stderr, "[%s] key %llu outside traversal range [%llu,%llu)\n",
                   where, (unsigned long long)key, (unsigned long long)lo,
                   (unsigned long long)hi);
      std::fflush(stderr);
      std::abort();
    }
    for (uint32_t i = 0; i < l->count && i < LK; ++i) {
      if (l->keys[i] < lo || l->keys[i] >= hi) {
        std::fprintf(stderr,
                     "[%s] leaf holds %llu, outside [%llu,%llu) (key %llu)\n",
                     where, (unsigned long long)l->keys[i],
                     (unsigned long long)lo, (unsigned long long)hi,
                     (unsigned long long)key);
        std::fflush(stderr);
        std::abort();
      }
    }
  }
#endif

  static bool safe_for_insert(NodeBase* n) {
    return n->is_leaf ? n->count < LK : n->count < IK;
  }

  // Top-down write coupling. Every lock is taken parent-before-child, so the
  // global lock order is the tree order and the descent cannot deadlock against
  // a concurrent read-coupled traversal.
  bool insert_smo(uint64_t key, uint64_t val) {
    NodeBase* locked[40];
    int n = 0;
    uint64_t lo = 0, hi = ~0ull;
    sentinel_->lock.write_lock();
    locked[n++] = sentinel_;
    NodeBase* node = sentinel_->child[0];
    for (;;) {
      node->lock.write_lock();
      if (safe_for_insert(node)) {
        for (int i = 0; i < n; ++i) locked[i]->lock.write_unlock();
        n = 0;
      }
      locked[n++] = node;
      if (node->is_leaf) break;
      node = descend_range(node, key, &lo, &hi);
    }

    Leaf* leaf = static_cast<Leaf*>(locked[n - 1]);
#ifdef OPAL_DEBUG_RANGE
    if (n >= 2 && descend(locked[n - 2], key) != locked[n - 1]) {
      std::fprintf(stderr, "[smo] path parent does not point at leaf\n");
      std::fflush(stderr);
      std::abort();
    }
    check_leaf_range(leaf, key, lo, hi, "smo");
#endif
    bool inserted = true;
    uint64_t up_key = 0;
    NodeBase* up_right = nullptr;

    if (leaf->count < LK) {
      inserted = leaf_insert(leaf, key, val);
    } else {
      Leaf* r = new_leaf();
      const uint32_t mid = LK / 2;
      r->count = LK - mid;
      memcpy(r->keys, leaf->keys + mid, sizeof(uint64_t) * r->count);
      memcpy(r->vals, leaf->vals + mid, sizeof(uint64_t) * r->count);
      leaf->count = mid;
      up_key = r->keys[0];
      inserted = (key < up_key) ? leaf_insert(leaf, key, val)
                                : leaf_insert(r, key, val);
      up_right = r;
    }

    int idx = n - 1;
    while (up_right != nullptr && idx > 0) {
      NodeBase* left = locked[idx];
      --idx;
      NodeBase* p = locked[idx];
      if (p == sentinel_) {
        Inner* nr = new_inner();
        nr->count = 1;
        nr->keys[0] = up_key;
        nr->child[0] = left;
        nr->child[1] = up_right;
        sentinel_->child[0] = nr;
        up_right = nullptr;
      } else {
        Inner* pi = static_cast<Inner*>(p);
        if (pi->count < IK) {
          inner_insert(pi, up_key, up_right);
          up_right = nullptr;
        } else {
          Inner* r = new_inner();
          const uint32_t mid = IK / 2;
          const uint64_t sep = pi->keys[mid];
          r->count = IK - mid - 1;
          memcpy(r->keys, pi->keys + mid + 1, sizeof(uint64_t) * r->count);
          memcpy(r->child, pi->child + mid + 1,
                 sizeof(NodeBase*) * (r->count + 1));
          pi->count = mid;
          if (up_key < sep) inner_insert(pi, up_key, up_right);
          else inner_insert(r, up_key, up_right);
          up_key = sep;
          up_right = r;
        }
      }
    }
    for (int i = 0; i < n; ++i) locked[i]->lock.write_unlock();
    return inserted;
  }

  static void validate(NodeBase* n, uint64_t lo, uint64_t hi, int depth,
                       int* leaf_depth, std::string* err) {
    if (!err->empty()) return;
    char buf[256];
    if (n->is_leaf) {
      if (*leaf_depth < 0) *leaf_depth = depth;
      else if (*leaf_depth != depth) {
        snprintf(buf, sizeof(buf), "leaf depth %d != %d", depth, *leaf_depth);
        *err = buf;
        return;
      }
      if (n->count > LK) { *err = "leaf count > LK"; return; }
      const Leaf* l = static_cast<const Leaf*>(n);
      for (uint32_t i = 0; i < n->count; ++i) {
        if (l->keys[i] < lo || l->keys[i] >= hi) {
          snprintf(buf, sizeof(buf), "leaf key %llu outside [%llu,%llu)",
                   (unsigned long long)l->keys[i], (unsigned long long)lo,
                   (unsigned long long)hi);
          *err = buf;
          return;
        }
        if (i && l->keys[i - 1] >= l->keys[i]) { *err = "leaf keys unsorted"; return; }
      }
      return;
    }
    if (n->count > IK) { *err = "inner count > IK"; return; }
    const Inner* in = static_cast<const Inner*>(n);
    for (uint32_t i = 0; i < in->count; ++i) {
      if (in->keys[i] < lo || in->keys[i] >= hi) {
        snprintf(buf, sizeof(buf), "sep %llu outside [%llu,%llu) at depth %d",
                 (unsigned long long)in->keys[i], (unsigned long long)lo,
                 (unsigned long long)hi, depth);
        *err = buf;
        return;
      }
      if (i && in->keys[i - 1] >= in->keys[i]) { *err = "separators unsorted"; return; }
    }
    for (uint32_t i = 0; i <= in->count; ++i) {
      uint64_t clo = (i == 0) ? lo : in->keys[i - 1];
      uint64_t chi = (i == in->count) ? hi : in->keys[i];
      validate(in->child[i], clo, chi, depth + 1, leaf_depth, err);
      if (!err->empty()) return;
    }
  }

  static size_t count_keys(NodeBase* n) {
    if (n->is_leaf) return n->count;
    Inner* in = static_cast<Inner*>(n);
    size_t s = 0;
    for (uint32_t i = 0; i <= in->count; ++i) s += count_keys(in->child[i]);
    return s;
  }
};

} // namespace idx
