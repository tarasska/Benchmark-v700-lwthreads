/*
Treiber Stack with FC
*/
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <boost/fiber/all.hpp>

#include <nasl/yield.hpp>

#include "globals_extern.h"

using namespace std;

typedef intptr_t skey_t;

#define CACHE_LINE_SIZE 128
#define FC_MAX_THREADS  2048
#define FC_THRESHOLD    2

template <typename K>
struct mstack_node {
   K key;
   mstack_node* next;

   explicit mstack_node(K k) : key(k), next(nullptr) {}
};

enum class FCOperationType : int {
   NONE = 0,
   PUSH,
   POP,
   FIND
};

enum class FCStatus : int {
   EMPTY    = 0,
   PUSHED   = 1,
   FINISHED = 2
};

template <typename K>
struct alignas(CACHE_LINE_SIZE) fc_request {
   volatile FCOperationType type;
   volatile skey_t          key;
   volatile FCStatus        status;
   K*                       result_ptr;
   volatile bool            result_valid;
   volatile int             pos;


   fc_request()
       : type(FCOperationType::NONE)
       , key(0)
       , status(FCStatus::EMPTY)
       , result_ptr(nullptr)
       , result_valid(false)
       , pos(-1)
   {}
};

template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack {
private:
    alignas(CACHE_LINE_SIZE) atomic<int> combiner_lock{0};

    alignas(CACHE_LINE_SIZE) fc_request<K> thread_slots[FC_MAX_THREADS];

    alignas(CACHE_LINE_SIZE) std::vector<K> stack{};

public:
    mstack() {}

    ~mstack() {
    }

    K* find(const int tid, skey_t key) {
        fc_request<K>& req = thread_slots[tid];
        K result_storage{};
        req.result_ptr   = &result_storage;
        req.result_valid = false;
        req.key          = key;
        req.type         = FCOperationType::FIND;
        handleRequest(tid, req);
        if (req.result_valid) {
            return new K(result_storage);
        }
        return nullptr;
    }

    unique_ptr<K> push(const int tid, skey_t key) {
        fc_request<K>& req = thread_slots[tid];
        K result_storage = static_cast<K>(key);
        req.result_ptr   = &result_storage;
        req.result_valid = false;
        req.key          = key;
        req.type         = FCOperationType::PUSH;

        handleRequest(tid, req);

        return make_unique<K>(result_storage);
    }

    unique_ptr<K> pop(const int tid) {
        fc_request<K>& req = thread_slots[tid];
        K result_storage{};
        req.result_ptr   = &result_storage;
        req.result_valid = false;
        req.key          = 0;
        req.type         = FCOperationType::POP;

        handleRequest(tid, req);

        if (req.result_valid) {
            return make_unique<K>(result_storage);
        }
        return nullptr;
    }

    bool empty() const {
        return stack.empty();
    }

private:
    bool tryLock() {
        int expected = 0;
        return combiner_lock.load(std::memory_order_relaxed) != 0 && combiner_lock.compare_exchange_strong(
            expected, 1,
            memory_order_acquire,
            memory_order_relaxed
        );
    }

    void unlock() {
        combiner_lock.store(0, memory_order_release);
    }

    void handleRequest(int tid, fc_request<K>& req) {
        req.status = FCStatus::PUSHED; // publication
        atomic_thread_fence(memory_order_release);
        //pub_array.addRequest(&req);
        while (true) {
            if (tryLock()) {
                combine();
                unlock();
                return;
            } else {
                while (req.status != FCStatus::FINISHED && combiner_lock.load(std::memory_order_relaxed) != 0) {
                    nasl::core::yield();
                }

                atomic_thread_fence(memory_order_acquire);
                if (req.status == FCStatus::FINISHED) {
                    return;
                }
            }
        }
    }

    void combine() {
        for (int t = 0; t < 16; ++t) {
            int ops = 0;
            for (int i = 0; i < g_max_threads; i++) {
                fc_request<K>& req = thread_slots[i];

                if (req.status == FCStatus::PUSHED) {
                    ++ops;
                    
                    if (req.type == FCOperationType::PUSH) {
                        stack.push_back(static_cast<K>(req.key));
                        req.result_valid = true;
                    } else if (req.type == FCOperationType::POP) {
                        if (stack.empty()) {
                            req.result_valid = false;
                        } else {
                            *(req.result_ptr) = stack.back();
                            req.result_valid = true;
                            stack.pop_back();
                        }
                    } else if (req.type == FCOperationType::FIND) {
                        auto it = std::find(stack.rbegin(), stack.rend(), req.key);
                        if (it != stack.rend()) {
                            *(req.result_ptr) = *it;
                            req.result_valid = true;
                        } else {
                            req.result_valid = false;
                        }
                    }

                    atomic_thread_fence(memory_order_release);
                    req.status = FCStatus::FINISHED;
                }
            }


            if (ops < FC_THRESHOLD) {
                break;  // not enough pending work
            }
        }
    }

    mstack(const mstack&) = delete;
    mstack& operator=(const mstack&) = delete;
};
