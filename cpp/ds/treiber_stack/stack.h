/*   
 *   Updated Treiber stack from ASCYLIB
 */
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <boost/fiber/all.hpp>
#include <immintrin.h>

using namespace std;

typedef intptr_t skey_t;

#define CACHE_LINE_SIZE 128

/**
 * Exponential backoff
 */
static constexpr uint64_t BASE_SPIN_COUNT = 100;
static constexpr uint64_t MAX_SPIN_COUNT = 10000;
static constexpr int SPIN_THRESHOLD = 8;

inline uint64_t f(int tries) {
    uint64_t spin_count = BASE_SPIN_COUNT * (1ULL << tries);  // 100 * 2^tries
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

template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack
{
    std::atomic<tagged_ptr<mstack_node<K>>> top;

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
            boost::this_fiber::yield();
        }
        return K{};
    }

    unique_ptr<K> push(const int tid, skey_t key) {
        mstack_node<K>* new_node = new mstack_node<K>(key);
        // mstack_node<K>* expected = top.load(memory_order_relaxed);
        // do {
        //     new_node->next = expected;
        //     tries++;
        //     if (tries < SPIN_THRESHOLD) {
        //         spin(f(tries));
        //     } else {
        //         boost::this_fiber::yield();
        //     }
        // } while (!top.compare_exchange_weak(
        //     expected, 
        //     new_node,
        //     memory_order_release,
        //     memory_order_relaxed
        // ));
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
            tries++;
            if (tries < SPIN_THRESHOLD) {
                spin(f(tries));
            } else {
                boost::this_fiber::yield();
            }
        }
        return std::make_unique<K>(key);
    }

    unique_ptr<K> pop(const int tid) {
        // int tries = 0;
        // mstack_node<K>* expected = top.load(memory_order_acquire);
        // mstack_node<K>* new_top;
        
        // do {
        //     if (expected == nullptr) {
        //         return nullptr;
        //     }
        //     // new_top = expected->next.load(memory_order_relaxed);
        //     new_top = expected->next;

        //     boost::this_fiber::yield();
        //     tries++;
        //     if (tries < SPIN_THRESHOLD) {
        //         spin(f(tries));
        //     } else {
        //         boost::this_fiber::yield();
        //     }
        // } while (!top.compare_exchange_weak(
        //     expected,
        //     new_top,
        //     memory_order_release,
        //     memory_order_acquire));
        
        // auto result = expected->key;
        // delete expected;
        // return std::make_unique<K>(result);
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
            tries++;
            if (tries < SPIN_THRESHOLD) {
                spin(f(tries));
            } else {
                boost::this_fiber::yield();
            }
        }
    }

    bool empty() const {
        return top.load(std::memory_order_acquire).ptr == nullptr;
    }

    mstack(const mstack&) = delete;
    mstack& operator=(const mstack&) = delete;
};