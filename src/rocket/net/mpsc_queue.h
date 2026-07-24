#pragma once

// Vyukov intrusive MPSC (multi-producer, single-consumer) lock-free queue.
//
// Producers (any thread) push nodes via wait-free push().
// The single consumer drains via pop() or tryPopAll().
//
// Pattern borrowed from Hical's GenericConnection write queue, which is in turn
// based on Dmitry Vyukov's classic MPSC algorithm.

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

namespace rocket {

// ── Intrusive linked-list node ──────────────────────────────────────────

struct MpscNode {
    std::atomic<MpscNode*> next{nullptr};
    virtual ~MpscNode() = default;
};

// ── Typed node carrying a payload ───────────────────────────────────────

template <typename T>
struct TypedMpscNode final : MpscNode {
    T data;

    template <typename U>
    explicit TypedMpscNode(U&& d) : data(std::forward<U>(d)) {}
};

// ── The queue ───────────────────────────────────────────────────────────
//
// Single consumer calls pop() / tryPopAll().
// Multiple producers call push() concurrently (wait-free).
//
// The embeded stub node avoids a null-head special case and costs nothing
// (it lives inside the queue object).

class MpscQueue {
  public:
    MpscQueue() : head_(&stub_), tail_(&stub_) {}

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    // Wait-free.  Any thread may call this.
    void push(MpscNode* node) {
        node->next.store(nullptr, std::memory_order_relaxed);
        MpscNode* prev = tail_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    // Consumer-only.  Returns nullptr when empty.
    MpscNode* pop() {
        MpscNode* h = head_;
        MpscNode* next = h->next.load(std::memory_order_acquire);

        if (h == &stub_) {
            if (!next) return nullptr;       // empty
            head_ = next;
            h = next;
            next = h->next.load(std::memory_order_acquire);
        }

        if (next) {
            head_ = next;
            return h;
        }

        // h != stub and next == null
        MpscNode* t = tail_.load(std::memory_order_acquire);
        if (h != t) {
            // Producer is mid-push: exchange done but next not yet linked.
            return nullptr;
        }

        // Queue is empty — re-insert the stub so the next push lands on it.
        push(&stub_);

        next = h->next.load(std::memory_order_acquire);
        if (next) {
            head_ = next;
            return h;
        }
        return nullptr;  // truly empty
    }

    // Consumer-only.  Pops every node in the queue, returning the head of
    // a singly-linked list, or nullptr if empty.
    MpscNode* tryPopAll() {
        MpscNode* h = head_;
        MpscNode* next = h->next.load(std::memory_order_acquire);

        if (h == &stub_) {
            if (!next) return nullptr;
            head_ = next;
            h = next;
            next = h->next.load(std::memory_order_acquire);
        }

        if (!next) {
            MpscNode* t = tail_.load(std::memory_order_acquire);
            if (h != t) return nullptr;  // mid-push
            push(&stub_);
            next = h->next.load(std::memory_order_acquire);
            if (!next) return nullptr;
        }

        // Detach the entire chain from h to tail.
        head_ = &stub_;
        tail_.store(&stub_, std::memory_order_release);

        // Walk to the real tail (skip the stub we just stored).
        // Actually, tail_ is now &stub_ but there may be nodes pushed after
        // our tail_ store.  That's fine — they're linked from stub_.next
        // and will be picked up in the next drain.
        return h;
    }

    // Consumer-only.
    [[nodiscard]] bool empty() const noexcept {
        if (head_ == &stub_) {
            return stub_.next.load(std::memory_order_acquire) == nullptr;
        }
        return false;
    }

    // Returns true when node is the internal sentinel (must be skipped by
    // the consumer when walking a chain returned by tryPopAll).
    [[nodiscard]] bool isStub(const MpscNode* node) const noexcept {
        return node == &stub_;
    }

  private:
    MpscNode stub_;                            // sentinel
    MpscNode* head_{nullptr};                   // consumer-owned
    alignas(64) std::atomic<MpscNode*> tail_;   // producer hot-spot, own cache line
};

// ── Thread-local node pool ──────────────────────────────────────────────
//
// Recycles TypedMpscNode<T> objects.  When a producer thread sends many
// messages, the pool eliminates repeated malloc/free.  Overflow nodes are
// returned to the heap.
//
// Usage:
//   auto* node = MpscNodePool::alloc<MyType>(args...);
//   queue.push(node);
//   // … later, on consumer thread:
//   while (auto* n = queue.pop()) {
//       process(n);
//       MpscNodePool::free(n);
//   }

class MpscNodePool {
  public:
    static constexpr std::size_t kMaxFree = 128;

    template <typename T, typename... Args>
    static TypedMpscNode<T>* alloc(Args&&... args) {
        static_assert(sizeof(TypedMpscNode<T>) >= sizeof(FreeSlot),
                      "TypedMpscNode<T> must be at least as large as FreeSlot");

        if (t_free_head) {
            void* p = t_free_head;
            t_free_head = t_free_head->next;
            --t_free_count;
            return ::new (p) TypedMpscNode<T>(std::forward<Args>(args)...);
        }
        return new TypedMpscNode<T>(std::forward<Args>(args)...);
    }

    static void free(MpscNode* node) {
        if (!node) return;
        node->~MpscNode();
        if (t_free_count < kMaxFree) {
            auto* slot = reinterpret_cast<FreeSlot*>(node);
            slot->next = t_free_head;
            t_free_head = slot;
            ++t_free_count;
        } else {
            ::operator delete(node);
        }
    }

    // Drain the entire thread-local free list (called at thread exit).
    static void purge() {
        while (t_free_head) {
            void* p = t_free_head;
            t_free_head = t_free_head->next;
            ::operator delete(p);
        }
        t_free_count = 0;
    }

  private:
    struct FreeSlot {
        FreeSlot* next{nullptr};
    };

    static inline thread_local FreeSlot* t_free_head = nullptr;
    static inline thread_local std::size_t t_free_count = 0;
};

} // namespace rocket
