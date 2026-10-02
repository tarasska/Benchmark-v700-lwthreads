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

template <typename K>
struct elem_array 
{
    enum class CellState : int {
        NONE = 0,
        SETTING = 1,
        WAIT = 2,
        IN_PROGRESS = 3,
        DONE = 4
    };
    
    struct cell {
        std::atomic<CellState> state;
        volatile K value;

        cell() : state(CellState::NONE), value() {} 
    };

    std::array<cell, ELIMINATION_ARRAY_SIZE> arr;

    bool push(K value) {
        int ind = rand() % ELIMINATION_ARRAY_SIZE;
        int attempts = ELIMINATION_ATTEMPTS;
        while (attempts > 0) {
            auto expected_state = CellState::NONE;
            if (arr[ind].state.compare_exchange_weak(expected_state, CellState::SETTING, std::memory_order_release, std::memory_order_relaxed)) {
                arr[ind].value = value;
                arr[ind].state.store(CellState::WAIT, std::memory_order_release);
                uint64_t time_bound = nasl::util::TicksTimer::clock_ticks() + ELIMINATION_WAITING_TIME_NS;
                while (true) {
                    auto cur_state = arr[ind].state.load(std::memory_order_acquire);
                    if (cur_state == CellState::WAIT) {
                        if (nasl::util::TicksTimer::clock_ticks() > time_bound) {
                            auto expected_state = CellState::WAIT;
                            if (arr[ind].state.compare_exchange_weak(expected_state, CellState::NONE, std::memory_order_release, std::memory_order_relaxed)) {
                                return false;
                            }
                        }
                        break;
                    } else if (cur_state == CellState::DONE) {
                        arr[ind].state.store(CellState::NONE, std::memory_order_release);
                        return true;
                    }
                }
            }

            attempts--;
            ind = (ind + 1) % ELIMINATION_ARRAY_SIZE;
        }
        return false;
    }

    std::optional<K> pop() {
        int ind = rand() % ELIMINATION_ARRAY_SIZE;
        int attempts = ELIMINATION_ATTEMPTS;
        while (attempts > 0) {
            auto expected_state = CellState::WAIT;
            if (arr[ind].state.compare_exchange_strong(expected_state, CellState::IN_PROGRESS, std::memory_order_release, std::memory_order_acquire)) {
                K res = arr[ind].value;
                arr[ind].state.store(CellState::DONE, std::memory_order_release);
                return { res };
            }

            attempts--;
            ind = (ind + 1) % ELIMINATION_ARRAY_SIZE;
        }
        return std::nullopt;
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
        if (elem_arr.push(key)) {
            return std::make_unique<K>(key);
        }
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
        auto fast_pop_opt = elem_arr.pop();
        if (fast_pop_opt) {
            return std::make_unique<K>(*fast_pop_opt);
        }
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