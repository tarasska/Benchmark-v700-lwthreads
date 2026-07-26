#pragma once


#include <cstdint>
#include <atomic>
#include <memory>
#include <vector>


#include <nasl/yield.hpp>
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



template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack {
private:
    std::vector<K> stack;
    volatile int size;
#ifdef USE_COROUTINES
    boost::fibers::mutex lock;
#else
    std::mutex lock;
#endif    


public:
    mstack() : stack(), lock() {}


    ~mstack() {
    }


    K* find(const int tid, skey_t key) {
        lock.lock();

        auto it = std::find(stack.rbegin(), stack.rend(), key);
        K* result = (it != stack.rend()) ? new K(*it) : nullptr;

        lock.unlock();

        return result;
    }


    unique_ptr<K> push(const int tid, skey_t key) {
        lock.lock();
        stack.push_back(key);
        ++size;
        lock.unlock();

        return make_unique<K>(key);
    }


    unique_ptr<K> pop(const int tid) {
        lock.lock();
        unique_ptr<K> result = nullptr;
        if (!stack.empty()) {
            result = make_unique<K>(stack.back());
            stack.pop_back();
            --size;
        }
        lock.unlock();
        
        return result;
    }


    bool empty() const {
        //lock.lock();
        return stack.empty();
        //lock.unlock();
        //return empty;
    }


    mstack(const mstack&) = delete;
    mstack& operator=(const mstack&) = delete;
};