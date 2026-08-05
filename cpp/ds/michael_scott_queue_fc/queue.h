/*
*   Michael-Scott Queue with Flat Combining
*/
#pragma once


#include <cstdint>
#include <atomic>
#include <memory>
#include <boost/fiber/all.hpp>


using namespace std;


typedef intptr_t skey_t;


#define CACHE_LINE_SIZE 128
#define FC_MAX_THREADS  2048
#define FC_TRIES        64
#define FC_THRESHOLD    2

template <typename K>
struct mqueue_node {
   K key;
   mqueue_node* next;


   explicit mqueue_node(K k) : key(k), next(nullptr) {}
   mqueue_node() : key(K{}), next(nullptr) {}
};

enum class FCOperationType : int {
   NONE = 0,
   ENQUEUE,
   DEQUEUE,
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


   // -1 = not registered
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
struct alignas(CACHE_LINE_SIZE) mqueue {
private:
   mqueue_node<K>* queue_head;
   mqueue_node<K>* queue_tail;


   alignas(CACHE_LINE_SIZE) atomic<int> combiner_lock{0};
   fc_array<K> pub_array;
   alignas(CACHE_LINE_SIZE) fc_request<K> thread_slots[FC_MAX_THREADS];


public:
   mqueue() {
       mqueue_node<K>* dummy = new mqueue_node<K>();
       queue_head = dummy;
       queue_tail = dummy;
   }


   ~mqueue() {
       mqueue_node<K>* curr = queue_head;
       while (curr != nullptr) {
           mqueue_node<K>* temp = curr;
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
       req.type         = FCOperationType::ENQUEUE;
       handleRequest(tid, req);
       return make_unique<K>(result_storage);
   }


   unique_ptr<K> pop(const int tid) {
       fc_request<K>& req = thread_slots[tid];
       K result_storage{};
       req.result_ptr   = &result_storage;
       req.result_valid = false;
       req.key          = 0;
       req.type         = FCOperationType::DEQUEUE;
       handleRequest(tid, req);
       if (req.result_valid) {
           return make_unique<K>(result_storage);
       }
       return nullptr;
   }


   bool empty() const {
       return queue_head == queue_tail;
   }

private:
   /*
   * FC PART
   */
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
               pub_array.addRequest(&req);
               fc_request<K>* batch[FC_MAX_THREADS + 1];
               for (int t = 0; t < FC_TRIES; ++t) {
                   int count = pub_array.loadRequests(batch);
                   if (count == 0) {
                       break;
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
                   boost::this_fiber::yield();
               }
               unlock();
               return;
           } else {
               while (isLocked()) {
                   if (req.status == FCStatus::FINISHED) {
                       return;
                   }
                   boost::this_fiber::yield();
                   pub_array.addRequest(&req);
               }
               if (req.status == FCStatus::FINISHED) {
                   return;
               }
           }
       }
   }

   void executeSingle(fc_request<K>* req) {
       switch (req->type) {
           case FCOperationType::ENQUEUE:
               doSeqEnqueue(static_cast<K>(req->key));
               req->result_valid = true;
               break;

           case FCOperationType::DEQUEUE:
               doSeqDequeue(req);
               break;

           case FCOperationType::FIND:
               doSeqFind(req);
               break;

           default:
               break;
       }
   }

   void doSeqEnqueue(K key) {
       mqueue_node<K>* node = new mqueue_node<K>(key);
       queue_tail->next = node;
       queue_tail = node;
   }

   void doSeqDequeue(fc_request<K>* req) {
       if (queue_head == queue_tail) {
           req->result_valid = false;
           return;
       }
       mqueue_node<K>* old_head = queue_head;
       mqueue_node<K>* first = old_head->next;
       queue_head = first;
       *(req->result_ptr) = first->key;
       req->result_valid  = true;
       delete old_head;
   }


   void doSeqFind(fc_request<K>* req) {
       skey_t key = req->key;
       // Start from the first real element (after dummy)
       mqueue_node<K>* curr = queue_head->next;
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


   mqueue(const mqueue&) = delete;
   mqueue& operator=(const mqueue&) = delete;
};
