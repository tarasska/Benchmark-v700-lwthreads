/*
*   Michael-Scott Queue with Flat Combining
*   Based on the FC pattern from Hendler et al.
*   Adapted for Boost Fibers (coroutines) — no sleep, only yield.
*
*   Cache line = 128 bytes (Intel prefetches two 64-byte lines at once).
*   Memory barriers via std::atomic memory orderings.
*
*   FC protocol:
*     1. Thread publishes its request into a per-thread slot.
*     2. Thread tries to acquire the combiner lock.
*     3a. If acquired — becomes the combiner:
*         - Scans all slots, executes pending requests sequentially on the
*           underlying sequential queue (no CAS contention — single writer).
*         - After a pass, if there are still pending requests, does another
*           pass (up to TRIES times or until no work remains).
*         - Releases the lock.
*     3b. If not acquired — spins (with fiber yield) until its own request
*         is marked FINISHED, re-publishing if needed.
*/
#pragma once


#include <cstdint>
#include <atomic>
#include <memory>
#include <boost/fiber/all.hpp>


using namespace std;


typedef intptr_t skey_t;


#define CACHE_LINE_SIZE 128
#define FC_MAX_THREADS  512
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
   EMPTY    = 0,   // slot is free, no pending request
   PUSHED   = 1,   // request has been published
   FINISHED = 2    // combiner has completed the request
};

template <typename K>
struct alignas(CACHE_LINE_SIZE) fc_request {
   // Written by the owning thread, read by the combiner
   volatile FCOperationType type;
   volatile skey_t          key;


   // Written by the combiner, read by the owning thread
   volatile FCStatus        status;
   K*                       result_ptr;   // points to owning thread's result storage
   volatile bool            result_valid; // true if result_ptr holds a value


   // Position in the FC array (-1 = not yet registered)
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
           // Store fence: make sure the slot pointer is visible before
           // the combiner reads `length`.
           atomic_thread_fence(memory_order_release);
           req->pos = idx;
       }
   }


   // Load all slots that currently hold a pending request.
   // Returns count written into `out` (null-terminated).
   int loadRequests(fc_request<K>** out) {
       // Acquire fence pairs with the release in addRequest
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
   // ── Underlying sequential queue (only touched by the combiner) ──────
   mqueue_node<K>* queue_head;
   mqueue_node<K>* queue_tail;


   // ── Flat Combining machinery ────────────────────────────────────────
   alignas(CACHE_LINE_SIZE) atomic<int> combiner_lock{0};
   fc_array<K> pub_array;


   // Per-thread request slots (thread-local via tid index)
   alignas(CACHE_LINE_SIZE) fc_request<K> thread_slots[FC_MAX_THREADS];


public:
   mqueue() {
       // Initialize with a dummy node (standard MS queue technique)
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


   // ── Public API (matches queue interface) ─────────────────────────────


   K* find(const int tid, skey_t key) {
       fc_request<K>& req = thread_slots[tid];
       K result_storage{};
       req.result_ptr   = &result_storage;
       req.result_valid = false;
       req.key          = key;
       req.type         = FCOperationType::FIND;


       // Publish & combine
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
   // ── FC protocol ─────────────────────────────────────────────────────


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
       // 1. Mark request as pending
       req.status = FCStatus::PUSHED;
       // Store fence: ensure status=PUSHED is visible before the combiner
       // scans the array.
       atomic_thread_fence(memory_order_release);


       // 2. Register in the publication array (idempotent)
       pub_array.addRequest(&req);


       // 3. Try to become the combiner
       while (true) {
           if (tryLock()) {
               // ── I am the combiner ───────────────────────────────────
               // Re-publish in case it wasn't visible yet
               pub_array.addRequest(&req);


               fc_request<K>* batch[FC_MAX_THREADS + 1];


               for (int t = 0; t < FC_TRIES; ++t) {
                   int count = pub_array.loadRequests(batch);


                   if (count == 0) {
                       break;  // no pending work
                   }


                   // Execute all pending requests sequentially
                   for (int i = 0; i < count; ++i) {
                       fc_request<K>* r = batch[i];
                       executeSingle(r);
                       // Mark finished with release semantics so the
                       // waiting fiber sees the result.
                       atomic_thread_fence(memory_order_release);
                       r->status = FCStatus::FINISHED;
                   }


                   // If fewer requests than threshold, no point looping
                   if (count < FC_THRESHOLD) {
                       break;
                   }


                   // Yield to let more requests accumulate
                   boost::this_fiber::yield();
               }


               unlock();
               return;
           } else {
               // ── I am NOT the combiner ───────────────────────────────
               // Spin-wait (with fiber yield) until my request is done
               // or the lock is released so I can retry.
               while (isLocked()) {
                   if (req.status == FCStatus::FINISHED) {
                       return;  // combiner handled my request
                   }
                   boost::this_fiber::yield();
                   // Re-publish to make sure the combiner sees us
                   pub_array.addRequest(&req);
               }


               // Lock was released — check if my request was handled
               if (req.status == FCStatus::FINISHED) {
                   return;
               }
               // Otherwise loop back and try to become combiner
           }
       }
   }


   // ── Sequential queue operations (called only by the combiner) ───────


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
           // Queue is empty (only dummy node)
           req->result_valid = false;
           return;
       }
      
       // The first real element is head->next
       mqueue_node<K>* old_head = queue_head;
       mqueue_node<K>* first = old_head->next;
      
       // Move head forward (first becomes the new dummy)
       queue_head = first;
      
       *(req->result_ptr) = first->key;
       req->result_valid  = true;
      
       // Delete the old dummy node
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
