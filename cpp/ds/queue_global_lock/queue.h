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
struct alignas(CACHE_LINE_SIZE) mqueue {
private:
    std::deque<K> queue;
    volatile int size;
#ifdef USE_COROUTINES
    boost::fibers::mutex lock;
#else
    std::mutex lock;
#endif    


public:
    mqueue() : queue(), lock() {}


    ~mqueue() {
    }


    K* find(const int tid, skey_t key) {
        lock.lock();

        auto it = std::find(queue.begin(), queue.end(), key);
        K* result = (it != queue.end()) ? new K(*it) : nullptr;

        lock.unlock();

        return result;
    }


    unique_ptr<K> push(const int tid, skey_t key) {
        lock.lock();
        queue.push_back(key);
        ++size;
        lock.unlock();

        return make_unique<K>(key);
    }


    unique_ptr<K> pop(const int tid) {
        lock.lock();
        unique_ptr<K> result = nullptr;
        if (!queue.empty()) {
            result = make_unique<K>(queue.front());
            queue.pop_front();
            --size;
        }
        lock.unlock();
        
        return result;
    }


    bool empty() const {
        //lock.lock();
        return size == 0;
        //lock.unlock();
        //return empty;
    }


    mqueue(const mqueue&) = delete;
    mqueue& operator=(const mqueue&) = delete;
};