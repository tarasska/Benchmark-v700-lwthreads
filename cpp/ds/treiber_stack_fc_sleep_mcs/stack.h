/*
Treiber Stack with FC
*/
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>

#include <boost/fiber/all.hpp>

#include <nasl/util/statefull_backoff.hpp>
#include <nasl/yield.hpp>
#include "../nasl_boost_fibers/suspendable.hpp"

using namespace std;

typedef intptr_t skey_t;
typedef nasl::core::DefaultSuspendData suspend_t;
typedef nasl::util::backoff::BinaryBackoffPolicy<suspend_t> BackoffPolicy;

#define CACHE_LINE_SIZE 128
#define FC_MAX_THREADS  2048
#define FC_TRIES        64
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
   
   std::atomic<fc_request*> next;
   std::atomic<bool>        locked;
   suspend_t                suspend_data{};


   fc_request()
       : type(FCOperationType::NONE)
       , key(0)
       , status(FCStatus::EMPTY)
       , result_ptr(nullptr)
       , result_valid(false)
       , next(nullptr)
       , locked(false)
   {}
};

template <typename K>
struct fc_array {
    std::atomic<fc_request<K>*> tail;

    fc_array() : tail(nullptr) {
    }

    template<typename StackOp>
    void executeRequest(fc_request<K>* req, StackOp op) {
        nasl::core::Suspendable<suspend_t>::init(&req->suspend_data);
        fc_request<K> *predecessor = tail.exchange(req);

        if (predecessor != nullptr) {
            //std::cout << "Adding: " << req << " Prev: " << req << std::endl;
            if (predecessor == req) {
                //std::cout << "ERROR, pred==req: " << req << std::endl; 
            }
            req->locked.store(true, std::memory_order_release);
            predecessor->next.store(req, std::memory_order_release);

            auto backoff_policy = BackoffPolicy::make(&req->suspend_data);
            while (req->locked.load(std::memory_order_acquire)) {
                backoff_policy.OnSpinWait();
            }
        } else {
            // nasl::core::Suspendable<suspend_t>::resume(&req->suspend_data); // Turn off suspending
            req->locked.store(false, std::memory_order_release);
            //std::cout << "Adding: " << req << " Set as head. " << std::endl;
            combine(req, op);
        }
    }

    template<typename StackOp>
    void combine(fc_request<K>* combiner_req, StackOp op) {

        fc_request<K>* cur = combiner_req;
        fc_request<K>* cur_tail = tail.load(std::memory_order_acquire);
        while (cur != cur_tail) {
            op(cur);
        
            fc_request<K>* next = nullptr;
            while (next == nullptr) {
                next = cur->next.load(std::memory_order_acquire);
                //std::cout << "Next cur:" << cur << std::endl;
            }
            cur = next;
        }
        op(cur);
        
        fc_request<K>* next_combiner = cur->next.load(std::memory_order_acquire);
        if (next_combiner == nullptr) {
            if (tail.compare_exchange_strong(cur_tail, nullptr, std::memory_order_seq_cst, std::memory_order_acquire)) {
                //std::cout << "Drop tail 2: " << last << std::endl;

                return;
            }

            next_combiner = nullptr;
            do {
                next_combiner = cur->next.load(std::memory_order_acquire);
                //std::cout << "Load last.next:" << cur << std::endl;
            } while (next_combiner == nullptr);
        }
        next_combiner->locked.store(false, std::memory_order_release);
        nasl::core::Suspendable<suspend_t>::resume(&next_combiner->suspend_data); // No suspend
        
        //std::cout << "Combined nodes: " << j << "Now head: " << head.load(std::memory_order_acquire) << std::endl;
    }
};

template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack {
private:
   mstack_node<K>* stack_top;
   alignas(CACHE_LINE_SIZE) atomic<int> combiner_lock{0};
   fc_array<K> pub_array;
   alignas(CACHE_LINE_SIZE) fc_request<K> thread_requests[FC_MAX_THREADS];
   std::atomic<int> gen_counter{0};


public:
    mstack() : stack_top(nullptr) {}

    ~mstack() {
        mstack_node<K>* curr = stack_top;
        while (curr != nullptr) {
            mstack_node<K>* temp = curr;
            curr = curr->next;
            delete temp;
        }
    }

    K* find(const int tid, skey_t key) {
        fc_request<K>& req = thread_requests[tid];
        K result_storage{};
        req.result_ptr   = &result_storage;
        req.result_valid = false;
        req.key          = key;
        req.type         = FCOperationType::FIND;
        req.locked.store(true, std::memory_order_release);
        
        handleRequest(tid, req);
        if (req.result_valid) {
            return new K(result_storage);
        }
        return nullptr;
    }

    unique_ptr<K> push(const int tid, skey_t key) {
        fc_request<K>& req = thread_requests[tid];
        K result_storage = static_cast<K>(key);
        req.result_ptr   = &result_storage;
        req.result_valid = false;
        req.key          = key;
        req.type         = FCOperationType::PUSH;
        req.next.store(nullptr, std::memory_order_release);
        req.locked.store(true, std::memory_order_release);
        
        handleRequest(tid, req);

        return make_unique<K>(result_storage);
    }

    unique_ptr<K> pop(const int tid) {
        fc_request<K>& req = thread_requests[tid];
        K result_storage{};
        req.result_ptr   = &result_storage;
        req.result_valid = false;
        req.key          = 0;
        req.type         = FCOperationType::POP;
        req.locked.store(true, std::memory_order_release);
        
        handleRequest(tid, req);

        if (req.result_valid) {
            return make_unique<K>(result_storage);
        }
        return nullptr;
    }

    bool empty() const {
        return stack_top == nullptr;
    }

private:
    bool tryLock() {
        int expected = 0;
        return combiner_lock.compare_exchange_strong(
            expected, 1,
            memory_order_acquire,
            memory_order_relaxed
        );
    }

    void unlock() {
        combiner_lock.store(0, memory_order_release);
    }

    bool isLocked() {
        return combiner_lock.load(memory_order_acquire) != 0;
    }

    void handleRequest(int tid, fc_request<K>& req) {
        //std::cout << "Handle req: " << &req << std::endl;
        req.status = FCStatus::PUSHED;
        atomic_thread_fence(memory_order_release);
        // pub_array.executeRequest(&req);
        // auto backoff_policy = BackoffPolicy::make(&req.suspend_data);
        // while (true) {
        //     if (req.status == FCStatus::FINISHED) {
        //         return; 
        //     }

        //     if (req.locked.load(std::memory_order_acquire)) {
        //         backoff_policy.OnSpinWait();
        //     } else {
        //         pub_array.combine(&req, [this](fc_request<K>* r) { this->executeRequest(r); });
        //         return;
        //     }
        // }
    }

    void executeRequest(fc_request<K>* r) {
        executeSingle(r);
        atomic_thread_fence(memory_order_release);
        r->status = FCStatus::FINISHED;
        r->locked.store(false, std::memory_order_release);
        nasl::core::Suspendable<suspend_t>::resume(&r->suspend_data);
    }

    void executeSingle(fc_request<K>* req) {
        switch (req->type) {
            case FCOperationType::PUSH:
                doSeqPush(static_cast<K>(req->key));
                req->result_valid = true;
                break;
            case FCOperationType::POP:
                doSeqPop(req);
                break;
            case FCOperationType::FIND:
                doSeqFind(req);
                break;
            default:
                break;
        }
    }

    void doSeqPush(K key) {
        mstack_node<K>* node = new mstack_node<K>(key);
        node->next = stack_top;
        stack_top  = node;
    }

    void doSeqPop(fc_request<K>* req) {
        if (stack_top == nullptr) {
            req->result_valid = false;
            return;
        }
        mstack_node<K>* node = stack_top;
        stack_top = node->next;


        *(req->result_ptr) = node->key;
        req->result_valid  = true;
        delete node;
    }

    void doSeqFind(fc_request<K>* req) {
        skey_t key = req->key;
        mstack_node<K>* curr = stack_top;
        while (curr != nullptr) {
            if (curr->key == static_cast<K>(key)) {
                *(req->result_ptr) = curr->key;
                req->result_valid  = true;
                return;
            }
            curr = curr->next;
        }
        req->result_valid = false;
    }

    mstack(const mstack&) = delete;
    mstack& operator=(const mstack&) = delete;
};
