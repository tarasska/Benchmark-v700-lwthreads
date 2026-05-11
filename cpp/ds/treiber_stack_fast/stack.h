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
    std::atomic<mstack_node<K>*> top;

    mstack() : top(nullptr) {}
    
    ~mstack() {
        mstack_node<K>* curr = top.load();
        while (curr != nullptr) {
            mstack_node<K>* temp = curr;
            curr = curr->next;
            delete temp;
        }
    }

    K* find(const int tid, skey_t key) {
        mstack_node<K>* curr = top.load(memory_order_acquire);
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
        int tries = 0;
        mstack_node<K>* new_node = new mstack_node<K>(key);
        mstack_node<K>* expected = top.load(memory_order_relaxed);
        do {
            new_node->next = expected;
            tries++;
            if (tries < SPIN_THRESHOLD) {
                spin(f(tries));
            } else {
                boost::this_fiber::yield();
            }
        } while (!top.compare_exchange_weak(
            expected, 
            new_node,
            memory_order_release,
            memory_order_relaxed
        ));
        return std::make_unique<K>(key);
    }

    unique_ptr<K> pop(const int tid) {
        int tries = 0;
        mstack_node<K>* expected = top.load(memory_order_acquire);
        mstack_node<K>* new_top;
        
        do {
            if (expected == nullptr) {
                return nullptr;
            }
            // new_top = expected->next.load(memory_order_relaxed);
            new_top = expected->next;
            tries++;
            if (tries < SPIN_THRESHOLD) {
                spin(f(tries));
            } else {
                boost::this_fiber::yield();
            }
        } while (!top.compare_exchange_weak(
            expected,
            new_top,
            memory_order_release,
            memory_order_acquire));
        
        auto result = expected->key;
        // W/O MEMORY RECLAMATION THIS SEGFAULTS
        // delete expected;
        return std::make_unique<K>(result);
    }

    bool empty() const {
        return top.load(std::memory_order_acquire).ptr == nullptr;
    }

    mstack(const mstack&) = delete;
    mstack& operator=(const mstack&) = delete;
};