/*
Treiber Stack with FC
*/
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>

#include <boost/fiber/all.hpp>

#include <nasl/util/statefull_backoff.hpp>
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
    std::atomic<fc_request<K>*> head;
    std::atomic<fc_request<K>*> tail;

    fc_array() : head(nullptr), tail(nullptr) {
    }

    void addRequest(fc_request<K>* req) {
        nasl::core::Suspendable<suspend_t>::init(&req->suspend_data);
        fc_request<K> *predecessor = tail.exchange(req);

        if (predecessor != nullptr) {
            //std::cout << "Adding: " << req << " Prev: " << req << std::endl;
            if (predecessor == req) {
                //std::cout << "ERROR, pred==req: " << req << std::endl; 
            }
            req->locked.store(true, std::memory_order_release);
            predecessor->next.store(req, std::memory_order_release);

            // auto backoff_policy = BackoffPolicy::make(&req->suspend_data);
            // while (req->locked.load(std::memory_order_acquire)) {
            //     backoff_policy.OnSpinWait();
            // }
        } else {
            nasl::core::Suspendable<suspend_t>::resume(&req->suspend_data); // Turn off suspending
            req->locked.store(false, std::memory_order_release);
            //std::cout << "Adding: " << req << " Set as head. " << std::endl;
            head.store(req, std::memory_order_release);
        }
    }

    template<typename StackOp>
    void combine(StackOp op) {
        fc_request<K>* cur_head = head.load(std::memory_order_acquire);
        if (cur_head == nullptr) {
            //std::cout << "Cur is empty" << std::endl;
            return;
        }

        fc_request<K>* last = tail.load(std::memory_order_acquire);
        //std::cout << "Head: " << cur_head << "; Tail: " << last << std::endl;
        // if (cur_head == last) {
        //     //std::cout << "Expected 1 size" << std::endl;
        //     // queue size == 1
        //     if (tail.compare_exchange_strong(last, nullptr, std::memory_order_seq_cst, std::memory_order_acquire)) {
        //         if (head.compare_exchange_strong(cur_head, nullptr)) {
        //             //std::cout << "Drop head " << cur_head << std::endl;
        //         }
        //         //std::cout << "Drop tail " << last << " Tail load: " << tail.load(std::memory_order_acquire) << std::endl;
        //         out[0] = last;
        //         out[1] = nullptr;
        //         return 1;
        //     }
        //     //std::cout << "Refresh tail, new last=" << last << std::endl;
        //     // otherwise reuse new 'last' value
        // }

        fc_request<K>* cur = cur_head;
        while (cur != last) {
            if (cur->status == FCStatus::PUSHED) {
                //std::cout << "Add ptr:" << cur << std::endl;
                op(cur);
            }
            fc_request<K>* next = nullptr;
            while (next == nullptr) {
                next = cur->next.load(std::memory_order_acquire);
                //std::cout << "Next cur:" << cur << std::endl;
            }
            cur = next;
        }
        if (cur->status == FCStatus::PUSHED) {
            //std::cout << "Add last ptr:" << cur << std::endl;
            op(cur);
        }
        
        fc_request<K>* last_next = last->next.load(std::memory_order_acquire);
        if (last_next == nullptr) {
            if (tail.compare_exchange_strong(last, nullptr, std::memory_order_seq_cst, std::memory_order_acquire)) {
                if (head.compare_exchange_strong(cur_head, nullptr)) {
                    //std::cout << "Drop head 2: " << cur_head << std::endl;
                } else {
                    //std::cout << "Drop head 2 failed: " << cur_head << std::endl;
                }
                //std::cout << "Drop tail 2: " << last << std::endl;

                return;
            }
            fc_request<K>* last_next = nullptr;
            do {
                last_next = last->next.load(std::memory_order_acquire);
                //std::cout << "Load last.next:" << cur << std::endl;
            } while (last_next == nullptr);
        }
        last_next->locked.store(false, std::memory_order_release);
        nasl::core::Suspendable<suspend_t>::resume(&last_next->suspend_data); // No suspend
        head.store(last_next, std::memory_order_release);
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
        pub_array.addRequest(&req);
        auto backoff_policy = BackoffPolicy::make(&req.suspend_data);
        while (true) {
            if (req.status == FCStatus::FINISHED) {
                return; 
            }

            if (req.locked.load(std::memory_order_acquire)) {
                backoff_policy.OnSpinWait();
            } else {
                pub_array.combine([this](fc_request<K>* r) { this->executeRequest(r); });
                return;
            }
        }
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
