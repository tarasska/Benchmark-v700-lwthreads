/*
* MS QUEUE LOCK FREE (Coroutines)
*/
#pragma once


#include <cstdint>
#include <atomic>
#include <memory>
#include <nasl/yield.hpp>


using namespace std;


typedef intptr_t skey_t;


#define CACHE_LINE_SIZE 128

template <typename K>
struct alignas(CACHE_LINE_SIZE) mqueue_node {
   K key;
   atomic<mqueue_node*> next;


   explicit mqueue_node(K k) : key(k), next(nullptr) {}
   mqueue_node() : key(K{}), next(nullptr) {}
};

template <typename K>
struct alignas(CACHE_LINE_SIZE) mqueue {
private:
   alignas(CACHE_LINE_SIZE) atomic<mqueue_node<K>*> head;
   alignas(CACHE_LINE_SIZE) atomic<mqueue_node<K>*> tail;
   // Keep the original dummy as the owner of the entire allocation chain.
   // A linked node's next is never reset, even after head advances past it.
   // No nodes may be reclaimed/reused while an operation can still reference them.
   mqueue_node<K>* const allocation_head;


public:
   mqueue() : allocation_head(new mqueue_node<K>()) {
       mqueue_node<K>* dummy = allocation_head;
       head.store(dummy, memory_order_relaxed);
       tail.store(dummy, memory_order_relaxed);
   }

   ~mqueue() {
       // Requires all operations to have finished (the benchmark joins workers).
       // This chain includes both popped nodes and the still-live queue.
       mqueue_node<K>* curr = allocation_head;
       while (curr != nullptr) {
           mqueue_node<K>* temp = curr;
           curr = curr->next.load(memory_order_relaxed);
           delete temp;
       }
   }

   unique_ptr<K> push(const int tid, skey_t key) {
       mqueue_node<K>* new_node = new mqueue_node<K>(static_cast<K>(key));


       while (true) {
           mqueue_node<K>* last = tail.load(memory_order_acquire);
           mqueue_node<K>* next = last->next.load(memory_order_acquire);

           if (last == tail.load(memory_order_relaxed)) {
               if (next == nullptr) {
                   if (last->next.compare_exchange_weak(
                           next, new_node,
                           memory_order_release,
                           memory_order_relaxed)) {
                       tail.compare_exchange_weak(
                           last, new_node,
                           memory_order_release,
                           memory_order_relaxed);
                       return make_unique<K>(static_cast<K>(key));
                   }
               } else {
                   tail.compare_exchange_weak(
                       last, next,
                       memory_order_release,
                       memory_order_relaxed);
               }
           }
           nasl::core::yield();
       }
   }

   unique_ptr<K> pop(const int tid) {
       while (true) {
           mqueue_node<K>* first = head.load(memory_order_acquire);
           mqueue_node<K>* last = tail.load(memory_order_acquire);
           mqueue_node<K>* next = first->next.load(memory_order_acquire);
           if (first == head.load(memory_order_relaxed)) {
               if (first == last) {
                   if (next == nullptr) {
                       return nullptr;
                   }
                   tail.compare_exchange_weak(
                       last, next,
                       memory_order_release,
                       memory_order_relaxed);
               } else {
                   K result = next->key;
                   if (head.compare_exchange_weak(
                           first, next,
                           memory_order_release,
                           memory_order_acquire)) {
                       // Retain first until destruction: other operations can
                       // still hold first/next, including across a fiber yield.
                       return make_unique<K>(result);
                   }
               }
           }
           nasl::core::yield();
       }
   }


   // TODO: Remove later?
   K* find(const int tid, skey_t key) {
       mqueue_node<K>* first = head.load(memory_order_acquire);
       mqueue_node<K>* curr = first->next.load(memory_order_acquire);
       while (curr != nullptr) {
           if (curr->key == static_cast<K>(key)) {
               return new K(curr->key);
           }
           curr = curr->next.load(memory_order_acquire);
           nasl::core::yield();
       }
       return nullptr;
   }

   bool empty() const {
       mqueue_node<K>* first = head.load(memory_order_acquire);
       mqueue_node<K>* next = first->next.load(memory_order_acquire);
       return (next == nullptr);
   }

   mqueue(const mqueue&) = delete;
   mqueue& operator=(const mqueue&) = delete;
};