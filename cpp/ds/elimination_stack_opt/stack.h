/*   
 *   Updated Treiber stack from ASCYLIB
 */
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <immintrin.h>
#include <optional>

#include <nasl/yield.hpp>
#include <nasl/util/ticks_timer.hpp>

using namespace std;

typedef intptr_t skey_t;

#define CACHE_LINE_SIZE 128

static const int ELIMINATION_ARRAY_SIZE = 20;
static const int ELIMINATION_ATTEMPTS = 3;
static const uint64_t ELIMINATION_WAITING_TIME_NS = 200;

/**
 * Exponential backoff
 */
static constexpr uint64_t BASE_SPIN_COUNT = 128;
static constexpr uint64_t MAX_SPIN_COUNT = 10000;
static constexpr int SPIN_THRESHOLD = 16;

inline uint64_t f(const int tid, const int tries) {
    uint64_t spin_count = (tid % BASE_SPIN_COUNT) + BASE_SPIN_COUNT * (1ULL << tries);  // 128 * 2^tries
    return std::min(spin_count, MAX_SPIN_COUNT);
}

inline void spin(uint64_t iterations) {
    for (volatile uint64_t i = 0; i < iterations; ++i) {
        _mm_pause();
    }
}

template <typename T>
struct tagged_ptr {
    T* ptr;
    uint64_t tag;
    tagged_ptr() : ptr(nullptr), tag(0) {}
    tagged_ptr(T* p, uint64_t t) : ptr(p), tag(t) {}
    bool operator==(const tagged_ptr& other) const {
        return ptr == other.ptr && tag == other.tag;
    }
    bool operator!=(const tagged_ptr& other) const {
        return !(*this == other);
    }
};


template <typename K>
struct mstack_node
{
  K key;
  struct mstack_node* next;

  explicit mstack_node(K k) : key(k), next(nullptr) {}
};

enum class Kind { 
    push,
    pop 
};

template <typename K>
struct Request {

    Request(Kind k, mstack_node<K>* n = nullptr) : kind(k), node(n) {}
    Kind kind;
    mstack_node<K>* node;
    mstack_node<K>* result = nullptr;
    std::atomic<int> status{0}; // 0: waiting, 1: matched, 2: same kind
};

template <typename K>
struct elem_array 
{
    std::array<std::atomic<Request<K>*>, ELIMINATION_ARRAY_SIZE> slots;

    static std::uint64_t random_number() {
        static std::atomic<std::uint64_t> seed{0x9e3779b97f4a7c15ULL};
        thread_local std::uint64_t state =
            seed.fetch_add(0x9e3779b97f4a7c15ULL, std::memory_order_relaxed);
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    bool exchange(Request<K>& self) {
        auto& slot = slots[random_number() % ELIMINATION_ARRAY_SIZE];
        Request<K>* other = slot.load(std::memory_order_acquire);
        if (other) {
            // Claim before inspecting: the owner may withdraw and destroy its
            // stack-allocated request at any time before this CAS succeeds.
            if (!slot.compare_exchange_strong(other, nullptr,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                return false;
            }
            if (other->kind == self.kind) {
                other->status.store(2, std::memory_order_release);
                return false;
            }
            if (self.kind == Kind::push) {
                other->result = self.node;
            } else {
                self.result = other->node;
            }
            other->status.store(1, std::memory_order_release);
            return true;
        }

        other = nullptr;
        if (!slot.compare_exchange_strong(other, &self,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return false;
        }
        for (std::size_t i = 0; i < 64; ++i) {
            const int status = self.status.load(std::memory_order_acquire);
            if (status != 0) {
                return status == 1;
            }
        }
        other = &self;
        if (slot.compare_exchange_strong(other, nullptr,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return false;
        }
        // A partner claimed the request. Keep it alive until the reply arrives.
        int status;
        while ((status = self.status.load(std::memory_order_acquire)) == 0) {
            nasl::core::yield();
        }
        return status == 1;
    }

};

template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack
{
    std::atomic<tagged_ptr<mstack_node<K>>> top;
    elem_array<K> elem_arr;

    mstack() : top(tagged_ptr<mstack_node<K>>(nullptr, 0)) {
        static_assert(sizeof(tagged_ptr<mstack_node<K>>) == 16,
                      "tagged_ptr must be 16 bytes");
    }
    
    ~mstack() {
        auto current = top.load(std::memory_order_relaxed);
        mstack_node<K>* curr = current.ptr;
        while (curr != nullptr) {
            mstack_node<K>* temp = curr;
            curr = curr->next;
            delete temp;
        }
    }

    K* find(const int tid, skey_t key) {
        auto current = top.load(memory_order_acquire);
        mstack_node<K>* curr = current.ptr;
        while (curr != nullptr) {
            if (curr->key == key) {
                return new K(curr->key);
            }
            curr = curr->next;
        }
        return nullptr;
    }

    unique_ptr<K> push(const int tid, skey_t key) {
        mstack_node<K>* new_node = new mstack_node<K>(key);
        tagged_ptr<mstack_node<K>> expected = top.load(std::memory_order_relaxed);
        int tries = 0;
        while (true) {
            new_node->next = expected.ptr;
            tagged_ptr<mstack_node<K>> desired(new_node, expected.tag + 1);
            if (top.compare_exchange_weak(
                    expected,
                    desired,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                break;
            }
            Request<K> request(Kind::push, new_node);
            if (elem_arr.exchange(request)) {
                return std::make_unique<K>(key);
            }
            tries++;
            if (tries < SPIN_THRESHOLD) {
                spin(f(tid, tries));
            } else {
                nasl::core::yield();
            }
        }
        return std::make_unique<K>(key);
    }

    unique_ptr<K> pop(const int tid) {
        tagged_ptr<mstack_node<K>> expected = top.load(std::memory_order_acquire);
        int tries = 0;
        while (true) {
            if (expected.ptr == nullptr) {
                return nullptr;
            }
            mstack_node<K>* new_top = expected.ptr->next;
            tagged_ptr<mstack_node<K>> desired(new_top, expected.tag + 1);
            if (top.compare_exchange_weak(
                    expected,
                    desired,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                K result = expected.ptr->key;
                delete expected.ptr;
                return std::make_unique<K>(result);
            }
            Request<K> request(Kind::pop);
            if (elem_arr.exchange(request)) {
                return std::make_unique<K>(request.result->key);
            }
            tries++;
            if (tries < SPIN_THRESHOLD) {
                spin(f(tid, tries));
            } else {
                nasl::core::yield();
            }
        }
    }

    bool empty() const {
        return top.load(std::memory_order_acquire).ptr == nullptr;
    }

    mstack(const mstack&) = delete;
    mstack& operator=(const mstack&) = delete;
};