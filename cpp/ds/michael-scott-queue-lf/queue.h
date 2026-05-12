/*
* MS QUEUE LOCK FREE (Coroutines)
*/
#pragma once


#include <cstdint>
#include <atomic>
#include <memory>
#include <boost/fiber/all.hpp>


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


public:
   mqueue() {
       mqueue_node<K>* dummy = new mqueue_node<K>();
       head.store(dummy, memory_order_relaxed);
       tail.store(dummy, memory_order_relaxed);
   }

   ~mqueue() {
       mqueue_node<K>* curr = head.load(memory_order_relaxed);
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
           boost::this_fiber::yield();
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
                       delete first;
                       return make_unique<K>(result);
                   }
               }
           }
           boost::this_fiber::yield();
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
           boost::this_fiber::yield();
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