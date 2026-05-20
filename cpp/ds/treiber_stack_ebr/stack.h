/*
*   Updated Treiber stack from ASCYLIB.
*
*/
#pragma once


#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <boost/fiber/all.hpp>


using namespace std;


typedef intptr_t skey_t;


#define CACHE_LINE_SIZE 128


template <typename K>
struct mstack_node
{
   K key;
   struct mstack_node* next;


   explicit mstack_node(K k) : key(k), next(nullptr) {}
};


// ---------------------------------------------------------------------------
//  Epoch-Based Reclamation (EBR)
// ---------------------------------------------------------------------------

#ifndef EBR_MAX_THREADS
#define EBR_MAX_THREADS 256
#endif

#ifndef EBR_RETIRE_THRESHOLD
#define EBR_RETIRE_THRESHOLD 64
#endif


template <typename Node>
class EBRManager {
private:
   struct alignas(CACHE_LINE_SIZE) ThreadState {
       std::atomic<uint64_t> local_epoch{0};
       // Number of nested critical-section entries on this thread. Needed
       // because multiple fibers on the same OS thread can be in the CS at
       // once (one yields while another is mid-operation).
       std::atomic<int> active_count{0};
       // 3 retire buckets, indexed by (epoch % 3).
       std::vector<Node*> retire_lists[3];
       // Counter used to throttle epoch-advance attempts.
       int retires_since_advance = 0;
       // Pad away from the next ThreadState.
       char pad[CACHE_LINE_SIZE];
   };


   // Global epoch starts at 1 so that 0 can be used as "quiescent".
   alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> global_epoch{1};
   ThreadState states[EBR_MAX_THREADS];


public:
   EBRManager() = default;


   EBRManager(const EBRManager&) = delete;
   EBRManager& operator=(const EBRManager&) = delete;


   // Enter critical section: announce current global epoch (only on the
   // outermost entry for this thread).
   void enter(int tid) {
       ThreadState& st = states[tid];
       // fetch_add returns previous value. If previous was 0, we are the
       // outermost activation on this thread -> publish epoch.
       int prev = st.active_count.fetch_add(1, std::memory_order_acq_rel);
       if (prev == 0) {
           uint64_t e = global_epoch.load(std::memory_order_acquire);
           st.local_epoch.store(e, std::memory_order_release);
           // Full fence so that subsequent reads of shared structure
           // (e.g. top.load()) cannot be reordered before the announce.
           std::atomic_thread_fence(std::memory_order_seq_cst);
       }
   }


   // Leave critical section. When the outermost fiber on this thread
   // exits, mark the thread quiescent.
   void exit(int tid) {
       ThreadState& st = states[tid];
       int prev = st.active_count.fetch_sub(1, std::memory_order_acq_rel);
       if (prev == 1) {
           st.local_epoch.store(0, std::memory_order_release);
       }
   }


   // Retire a node: it is logically deleted, but actual delete is deferred
   // until no thread can possibly hold a pointer to it.
   void retire(int tid, Node* node) {
       if (node == nullptr) return;
       ThreadState& st = states[tid];
       uint64_t e = global_epoch.load(std::memory_order_acquire);
       st.retire_lists[e % 3].push_back(node);
       if (++st.retires_since_advance >= EBR_RETIRE_THRESHOLD) {
           st.retires_since_advance = 0;
           tryAdvance(tid);
       }
   }


   // Try to advance the global epoch and free this thread's bucket that
   // is now safe.
   void tryAdvance(int tid) {
       uint64_t e = global_epoch.load(std::memory_order_acquire);
       // All active threads must already be at epoch e for advance to be
       // safe. Quiescent threads (local_epoch == 0) are fine.
       for (int i = 0; i < EBR_MAX_THREADS; ++i) {
           uint64_t le = states[i].local_epoch.load(std::memory_order_acquire);
           if (le != 0 && le != e) {
               return;
           }
       }
       // Try to bump the global epoch from e to e+1. If somebody else did
       // it already we still proceed to free our own oldest bucket below.
       global_epoch.compare_exchange_strong(
           e, e + 1,
           std::memory_order_acq_rel,
           std::memory_order_acquire);


       // After advancing past e, no live thread can hold pointers retired
       // during epoch e-1 or earlier (they all announced e, so they began
       // their operations no earlier than the start of epoch e, after
       // those nodes were already unlinked). We free the bucket two
       // epochs behind the new global epoch (= e - 1).
       // Index (e - 1) % 3 == (e + 2) % 3.
       ThreadState& st = states[tid];
       auto& bucket = st.retire_lists[(e + 2) % 3];
       for (Node* p : bucket) {
           delete p;
       }
       bucket.clear();
   }


   // Drain everything; called from the destructor of mstack when the
   // structure is being torn down. Safe only when no thread is operating
   // on the stack anymore (single-threaded teardown).
   void drainAll() {
       for (int i = 0; i < EBR_MAX_THREADS; ++i) {
           for (int j = 0; j < 3; ++j) {
               for (Node* p : states[i].retire_lists[j]) {
                   delete p;
               }
               states[i].retire_lists[j].clear();
           }
       }
   }


   ~EBRManager() {
       drainAll();
   }
};


// RAII guard for enter/exit so we never leak an active count even on
// exceptions or early returns.
template <typename Node>
struct EBRGuard {
   EBRManager<Node>& mgr;
   int tid;
   EBRGuard(EBRManager<Node>& m, int t) : mgr(m), tid(t) { mgr.enter(tid); }
   ~EBRGuard() { mgr.exit(tid); }
   EBRGuard(const EBRGuard&) = delete;
   EBRGuard& operator=(const EBRGuard&) = delete;
};


template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack
{
   atomic<mstack_node<K>*> top;
   EBRManager<mstack_node<K>> ebr;


   mstack() : top(nullptr) {}


   ~mstack() {
       mstack_node<K>* curr = top.load();
       while (curr != nullptr) {
           mstack_node<K>* temp = curr;
           curr = curr->next;
           delete temp;
       }
       ebr.drainAll();
   }


   K* find(const int tid, skey_t key) {
       EBRGuard<mstack_node<K>> guard(ebr, tid);
       mstack_node<K>* curr = top.load(memory_order_acquire);
       while (curr != nullptr) {
           if (curr->key == key) {
               return new K(curr->key);
           }
           curr = curr->next;
           boost::this_fiber::yield();
       }
       return nullptr;
   }


   unique_ptr<K> push(const int tid, skey_t key) {
       EBRGuard<mstack_node<K>> guard(ebr, tid);
       mstack_node<K>* new_node = new mstack_node<K>(key);
       mstack_node<K>* expected = top.load(memory_order_relaxed);


       do {
           new_node->next = expected;
           boost::this_fiber::yield();
       } while (!top.compare_exchange_weak(
           expected,
           new_node,
           memory_order_release,
           memory_order_relaxed
       ));


       return std::make_unique<K>(key);
   }


   unique_ptr<K> pop(const int tid) {
       EBRGuard<mstack_node<K>> guard(ebr, tid);
       mstack_node<K>* expected = top.load(memory_order_acquire);
       mstack_node<K>* new_top;


       do {
           if (expected == nullptr) {
               return nullptr;
           }
           // Safe under EBR: even if some other thread already unlinked
           // 'expected', it cannot have been reclaimed because we are in
           // a critical section under the same epoch.
           new_top = expected->next;


           boost::this_fiber::yield();
       } while (!top.compare_exchange_weak(
           expected,
           new_top,
           memory_order_release,
           memory_order_acquire));


       K result = expected->key;
       // Defer reclamation instead of deleting immediately. This is the
       // core of the ABA fix: even if our 'expected' pointer is briefly
       // reused for another allocation, EBR guarantees that NO node is
       // freed while any thread might still observe its address.
       ebr.retire(tid, expected);
       return std::make_unique<K>(result);
   }


   bool empty() const {
       return top.load(memory_order_acquire) == nullptr;
   }


   mstack(const mstack&) = delete;
   mstack& operator=(const mstack&) = delete;
};
