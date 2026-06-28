/*
*   Concurrent Stack protected by NASL MCS Lock with Fiber-aware Backoff.
*
*   Uses the MCS queue lock from nasl-tools/locks with DefaultBackoffPolicy
*   (NOP spin + yield) instead of BinaryBackoffPolicy (which uses Suspendable).
*
*   In multi-OS-thread mode (M threads × N fibers), the MCS queue links nodes
*   from fibers across different OS threads.  The BinaryBackoffPolicy with
*   Suspendable uses boost::fibers::promise for suspend/resume, which is NOT
*   safe across OS threads (promise/future are thread-local to the fiber
*   scheduler).  Therefore we use DefaultBackoffPolicy which only does NOP
*   spin + nasl::core::yield() — both are safe across OS threads.
*
*   We still keep DefaultSuspendData as the MCS node's SuspendData type because
*   McsLock::unlock() always calls Suspendable<SuspendData>::resume().  With
*   DefaultBackoffPolicy, suspend() is never called, so state_ptr stays at
*   kReadyForSuspend (0).  When resume() runs, it atomically exchanges to
*   kKeepActive (1), gets back 0, and since 0 > 1 is false, it skips the
*   promise dereference — making it a safe no-op.
*
*   IMPORTANT: initCtx() must be called exactly once per MCS node (in the
*   constructor), NOT before every lock().  Calling initCtx() before lock()
*   can corrupt the MCS queue if the node is still linked as a predecessor
*   from a concurrent operation on another OS thread.
*
*   Cache line = 128 bytes to avoid false sharing.
*/
#pragma once


#include <cstdint>
#include <atomic>
#include <memory>


// nasl lock infrastructure
#include <nasl/lock/mcs/mcs.hpp>
#include <nasl/lock/mutex_wrapper.hpp>
#include <nasl/util/statefull_backoff.hpp>


// We include the fiber Suspendable specialization because McsLock::unlock()
// calls Suspendable<DefaultSuspendData>::resume().  With DefaultBackoffPolicy,
// suspend() is never called, so resume() is always a no-op (safe across threads).
#include <nasl/benchmark/fibers/lock/suspendable.hpp>


#include <boost/fiber/all.hpp>


using namespace std;


typedef intptr_t skey_t;


#define CACHE_LINE_SIZE 128


template <typename K>
struct mstack_node {
   K key;
   mstack_node* next;


   explicit mstack_node(K k) : key(k), next(nullptr) {}
};


/*
* MCS lock configured with DefaultBackoffPolicy (no suspend/resume in backoff).
* Backoff strategy:
*   - Short waits: NOP spin (binary exponential backoff)
*   - Long waits:  nasl::core::yield() -> boost::this_fiber::yield()
*
* DefaultSuspendData is kept as the node type so that unlock()'s resume() call
* compiles and is a safe no-op (since suspend() was never called).
*
* This is safe for multi-OS-thread mode because:
*   1. yield() only affects the current fiber within its own OS thread
*   2. suspend() is never called, so no cross-thread promise/future usage
*   3. resume() in unlock() is a no-op when state_ptr == kReadyForSuspend
*/
using FiberMcsLock = nasl::core::McsLock<
   nasl::util::backoff::DefaultBackoffPolicy<>,
   nasl::core::DefaultSuspendData
>;


template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack {
private:
   mstack_node<K>* stack_top;


   // NASL MCS lock with yield-based backoff (no cross-thread suspend)
   FiberMcsLock lock_;


   // Per-fiber lock contexts (MCS node per fiber, indexed by tid)
   static constexpr int MAX_THREADS = 512;
   alignas(CACHE_LINE_SIZE) FiberMcsLock::LockCtxType ctx_[MAX_THREADS];


public:
   mstack() : stack_top(nullptr) {
       // Initialize all MCS nodes once — do NOT re-init before each lock()
       for (int i = 0; i < MAX_THREADS; ++i) {
           lock_.initCtx(ctx_[i]);
       }
   }


   ~mstack() {
       mstack_node<K>* curr = stack_top;
       while (curr != nullptr) {
           mstack_node<K>* temp = curr;
           curr = curr->next;
           delete temp;
       }
   }


   K* find(const int tid, skey_t key) {
       // NOTE: no initCtx here — already initialized in constructor
       lock_.lock(ctx_[tid]);


       mstack_node<K>* curr = stack_top;
       while (curr != nullptr) {
           if (curr->key == static_cast<K>(key)) {
               K* result = new K(curr->key);
               lock_.unlock(ctx_[tid]);
               return result;
           }
           curr = curr->next;
       }


       lock_.unlock(ctx_[tid]);
       return nullptr;
   }


   unique_ptr<K> push(const int tid, skey_t key) {
       mstack_node<K>* new_node = new mstack_node<K>(key);


       // NOTE: no initCtx here — already initialized in constructor
       lock_.lock(ctx_[tid]);


       new_node->next = stack_top;
       stack_top = new_node;


       lock_.unlock(ctx_[tid]);


       return make_unique<K>(key);
   }


   unique_ptr<K> pop(const int tid) {
       // NOTE: no initCtx here — already initialized in constructor
       lock_.lock(ctx_[tid]);


       if (stack_top == nullptr) {
           lock_.unlock(ctx_[tid]);
           return nullptr;
       }


       mstack_node<K>* node = stack_top;
       stack_top = node->next;
       K result = node->key;
       delete node;


       lock_.unlock(ctx_[tid]);


       return make_unique<K>(result);
   }


   bool empty() const {
       return stack_top == nullptr;
   }


   mstack(const mstack&) = delete;
   mstack& operator=(const mstack&) = delete;
};