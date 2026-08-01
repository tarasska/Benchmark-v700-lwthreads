/*
Treiber Stack with FC
*/
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <boost/fiber/all.hpp>

#include <nasl/yield.hpp>

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
struct fc_array {
   fc_request<K>* slots[FC_MAX_THREADS];
   atomic<int>    length{0};


   fc_array() {
       for (int i = 0; i < FC_MAX_THREADS; ++i) {
           slots[i] = nullptr;
       }
   }

   void addRequest(fc_request<K>* req) {
       if (req->pos == -1) {
           int idx = length.fetch_add(1, memory_order_relaxed);
           slots[idx] = req;
           atomic_thread_fence(memory_order_release);
           req->pos = idx;
       }
   }
   int loadRequests(fc_request<K>** out) {
       atomic_thread_fence(memory_order_acquire);
       int end = length.load(memory_order_relaxed);
       int j = 0;
       for (int i = 0; i < end; ++i) {
           fc_request<K>* r = slots[i];
           if (r != nullptr && r->status == FCStatus::PUSHED) {
               out[j++] = r;
           }
       }
       out[j] = nullptr;
       return j;
   }
};

template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack {
private:
   mstack_node<K>* stack_top;
   alignas(CACHE_LINE_SIZE) atomic<int> combiner_lock{0};
   fc_array<K> pub_array;
   alignas(CACHE_LINE_SIZE) fc_request<K> thread_slots[FC_MAX_THREADS];


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
       req.status = FCStatus::PUSHED;
       atomic_thread_fence(memory_order_release);
       pub_array.addRequest(&req);
       while (true) {
           if (tryLock()) {
               //pub_array.addRequest(&req);
               fc_request<K>* batch[FC_MAX_THREADS + 1];
               for (int t = 0; t < 4; ++t) {
                   int count = pub_array.loadRequests(batch);
                   if (count == 0) {
                       break;  // no pending work
                   }
                   for (int i = 0; i < count; ++i) {
                       fc_request<K>* r = batch[i];
                       executeSingle(r);
                       atomic_thread_fence(memory_order_release);
                       r->status = FCStatus::FINISHED;
                   }
                   if (count < FC_THRESHOLD) {
                       break;
                   }
                   //nasl::core::yield();
               }
               unlock();
               return;
           } else {
               while (isLocked()) {
                   if (req.status == FCStatus::FINISHED) {
                       return; 
                   }
                   nasl::core::yield();
                   //pub_array.addRequest(&req);
               }
               if (req.status == FCStatus::FINISHED) {
                   return;
               }
           }
       }
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
