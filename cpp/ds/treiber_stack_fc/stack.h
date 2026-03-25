/*   
 *   Updated Treiber stack from ASCYLIB
 */
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>

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

enum class RequestType {
    NONE,
    PUSH,
    POP,
    FIND
};

template <typename K>
struct request {
    RequestType type;
    int tid;
    skey_t key;                    
    std::unique_ptr<K>* result;   
    bool completed;              
    std::mutex mutex;
    std::condition_variable cv;

    request(RequestType t, int id, skey_t k, std::unique_ptr<K>* r)
        : type(t), tid(id), key(k), result(r), completed(false) {}
};

template <typename K>
struct alignas(CACHE_LINE_SIZE) mstack {
    std::atomic<mstack_node<K>*> top;
    std::vector<request<K>> request_queue;
    std::mutex queue_mutex;               
    std::atomic<bool> combining_in_progress{false};

    mstack() : top(nullptr) {}

    ~mstack() {
        mstack_node<K>* curr = top.load();
        while (curr != nullptr) {
            mstack_node<K>* temp = curr;
            curr = curr->next;
            delete temp;
        }
    }

    std::unique_ptr<K> find(const int tid, skey_t key) {
        std::unique_ptr<K> result;
        request<K> req(RequestType::FIND, tid, key, &result);
        submit_request(req);
        return result;
    }

    std::unique_ptr<K> push(const int tid, skey_t key) {
        std::unique_ptr<K> result = std::make_unique<K>(key);
        request<K> req(RequestType::PUSH, tid, key, &result);
        submit_request(req);
        return result;
    }

    std::unique_ptr<K> pop(const int tid) {
        std::unique_ptr<K> result;
        request<K> req(RequestType::POP, tid, 0, &result);
        submit_request(req);
        return result;
    }

    bool empty() const {
        return top.load(std::memory_order_acquire) == nullptr;
    }

    void submit_request(request<K>& req) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            request_queue.push_back(req);
        }
        while (try_combine()) {
            combine();
        } else {
            
        }
        std::unique_lock<std::mutex> lk(req.mutex);
        req.cv.wait(lk, [&req] { return req.completed; });
    }

    bool try_combine() {
        return combining_in_progress.exchange(true, std::memory_order_acq_rel) == false;
    }

    void combine() {
        std::vector<request<K>> local_queue;

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            local_queue = std::move(request_queue);
            request_queue.clear();
        }

        for (auto& req : local_queue) {
            switch (req.type) {
                case RequestType::PUSH:do_push(req.key);
                    break;
                case RequestType::POP:
                    *req.result = do_pop();
                    break;
                case RequestType::FIND:
                    *req.result = do_find(req.key);
                    break;
            }
            {
                std::lock_guard<std::mutex> lock(req.mutex);
                req.completed = true;
            }
            req.cv.notify_one();
        }

        combining_in_progress.store(false, std::memory_order_release);
    }

private:
    void do_push(skey_t key) {
        mstack_node<K>* new_node = new mstack_node<K>(static_cast<K>(key));
        mstack_node<K>* expected = top.load(std::memory_order_relaxed);

        do {
            new_node->next = expected;
        } while (!top.compare_exchange_weak(
            expected,
            new_node,
            std::memory_order_release,
            std::memory_order_relaxed
        ));
    }

    std::unique_ptr<K> do_pop() {
        mstack_node<K>* expected = top.load(std::memory_order_acquire);
        mstack_node<K>* new_top;

        do {
            if (expected == nullptr) {
                return nullptr;
            }
            new_top = expected->next;
        } while (!top.compare_exchange_weak(
            expected,
            new_top,
            std::memory_order_release,
            std::memory_order_acquire
        ));

        auto result = std::make_unique<K>(expected->key);
        delete expected;
        return result;
    }

    std::unique_ptr<K> do_find(skey_t key) {
        mstack_node<K>* curr = top.load(std::memory_order_acquire);
        while (curr != nullptr) {
            if (curr->key == key) {
                return std::make_unique<K>(curr->key);
            }
            curr = curr->next;
            boost::this_fiber::yield();
        }
        return nullptr;
    }

    mstack(const mstack&) = delete;
    mstack& operator=(const mstack&) = delete;
};