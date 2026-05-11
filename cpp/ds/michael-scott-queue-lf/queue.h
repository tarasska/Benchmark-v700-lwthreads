/*
*   Algorithm:
*     - Uses a dummy node to simplify empty queue handling
*     - Enqueue: CAS on tail->next, then CAS to advance tail
*     - Dequeue: CAS on head to advance past dummy node
*     - Helping: threads help advance tail if it's lagging
*/
#pragma once


#include <cstdint>
#include <atomic>
#include <memory>
#include <boost/fiber/all.hpp>


using namespace std;


typedef intptr_t skey_t;


#define CACHE_LINE_SIZE 128


// ─────────────────────────────────────────────────────────────────────────────
// Queue node (intrusive singly-linked list)
// ─────────────────────────────────────────────────────────────────────────────
template <typename K>
struct alignas(CACHE_LINE_SIZE) mqueue_node {
   K key;
   atomic<mqueue_node*> next;


   explicit mqueue_node(K k) : key(k), next(nullptr) {}
   mqueue_node() : key(K{}), next(nullptr) {}
};


// ─────────────────────────────────────────────────────────────────────────────
// Michael-Scott Lock-Free Queue
// ─────────────────────────────────────────────────────────────────────────────
template <typename K>
struct alignas(CACHE_LINE_SIZE) mqueue {
private:
   alignas(CACHE_LINE_SIZE) atomic<mqueue_node<K>*> head;
   alignas(CACHE_LINE_SIZE) atomic<mqueue_node<K>*> tail;


public:
   mqueue() {
       // Initialize with a dummy node
       mqueue_node<K>* dummy = new mqueue_node<K>();
       head.store(dummy, memory_order_relaxed);
       tail.store(dummy, memory_order_relaxed);
   }


   ~mqueue() {
       // Delete all remaining nodes including dummy
       mqueue_node<K>* curr = head.load(memory_order_relaxed);
       while (curr != nullptr) {
           mqueue_node<K>* temp = curr;
           curr = curr->next.load(memory_order_relaxed);
           delete temp;
       }
   }

   /**
    * Enqueue (push) a key to the back of the queue.
    * Returns a unique_ptr to the enqueued key.
    */
   unique_ptr<K> push(const int tid, skey_t key) {
       mqueue_node<K>* new_node = new mqueue_node<K>(static_cast<K>(key));


       while (true) {
           mqueue_node<K>* last = tail.load(memory_order_acquire);
           mqueue_node<K>* next = last->next.load(memory_order_acquire);


           // Check if tail is still consistent
           if (last == tail.load(memory_order_relaxed)) {
               if (next == nullptr) {
                   // Tail is pointing to the last node, try to link new node
                   if (last->next.compare_exchange_weak(
                           next, new_node,
                           memory_order_release,
                           memory_order_relaxed)) {
                       // Successfully linked, now try to advance tail
                       // (it's okay if this fails — another thread will help)
                       tail.compare_exchange_weak(
                           last, new_node,
                           memory_order_release,
                           memory_order_relaxed);
                       return make_unique<K>(static_cast<K>(key));
                   }
               } else {
                   // Tail is lagging behind, help advance it
                   tail.compare_exchange_weak(
                       last, next,
                       memory_order_release,
                       memory_order_relaxed);
               }
           }
           // Yield to other fibers before retrying
           boost::this_fiber::yield();
       }
   }


   /**
    * Dequeue (pop) a key from the front of the queue.
    * Returns a unique_ptr to the dequeued key, or nullptr if queue is empty.
    */
   unique_ptr<K> pop(const int tid) {
       while (true) {
           mqueue_node<K>* first = head.load(memory_order_acquire);
           mqueue_node<K>* last = tail.load(memory_order_acquire);
           mqueue_node<K>* next = first->next.load(memory_order_acquire);


           // Check if head is still consistent
           if (first == head.load(memory_order_relaxed)) {
               if (first == last) {
                   // Queue appears empty or tail is lagging
                   if (next == nullptr) {
                       // Queue is truly empty
                       return nullptr;
                   }
                   // Tail is lagging, help advance it
                   tail.compare_exchange_weak(
                       last, next,
                       memory_order_release,
                       memory_order_relaxed);
               } else {
                   // Read value before CAS, otherwise another dequeue might
                   // free the node
                   K result = next->key;


                   // Try to advance head
                   if (head.compare_exchange_weak(
                           first, next,
                           memory_order_release,
                           memory_order_acquire)) {
                       // Successfully dequeued, free the old dummy node
                       delete first;
                       return make_unique<K>(result);
                   }
               }
           }
           // Yield to other fibers before retrying
           boost::this_fiber::yield();
       }
   }


   /**
    * Find a key in the queue (linear search).
    * Returns a pointer to the found key, or nullptr if not found.
    * Note: This is not a typical queue operation but provided for compatibility.
    */
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


   /**
    * Check if the queue is empty.
    */
   bool empty() const {
       mqueue_node<K>* first = head.load(memory_order_acquire);
       mqueue_node<K>* next = first->next.load(memory_order_acquire);
       return (next == nullptr);
   }


   // Disable copy constructor and assignment
   mqueue(const mqueue&) = delete;
   mqueue& operator=(const mqueue&) = delete;
};