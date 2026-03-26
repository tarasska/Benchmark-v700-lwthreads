#pragma once

#include <cstdint>
#include <atomic>
#include <memory>

#include <nasl/lock/mcs/mcs.hpp>
#include <nasl/lock/mutex_wrapper.hpp>
#include <nasl/util/statefull_backoff.hpp>
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

using FiberMcsLock = nasl::core::McsLock<
   nasl::util::backoff::BinaryBackoffPolicy<nasl::core::DefaultSuspendData>,
   nasl::core::DefaultSuspendData
>;

template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack {
private:
   mstack_node<K>* stack_top;
   FiberMcsLock lock_;
   static constexpr int MAX_THREADS = 512;
   alignas(CACHE_LINE_SIZE) FiberMcsLock::LockCtxType ctx_[MAX_THREADS];

public:
   mstack() : stack_top(nullptr) {
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
       lock_.initCtx(ctx_[tid]);
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
       return K{};
   }

   unique_ptr<K> push(const int tid, skey_t key) {
       mstack_node<K>* new_node = new mstack_node<K>(key);

       lock_.initCtx(ctx_[tid]);
       lock_.lock(ctx_[tid]);

       new_node->next = stack_top;
       stack_top = new_node;

       lock_.unlock(ctx_[tid]);

       return make_unique<K>(key);
   }

   unique_ptr<K> pop(const int tid) {
       lock_.initCtx(ctx_[tid]);
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
