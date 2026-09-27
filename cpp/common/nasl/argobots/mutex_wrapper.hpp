#pragma once

#include <abt.h>
#include <memory>

#include <nasl/types.hpp>

class AbtMutexWrapper {
  private:
    std::unique_ptr<ABT_mutex> mutex_;
  public:

    explicit AbtMutexWrapper() {
        mutex_ = std::unique_ptr<ABT_mutex>(new ABT_mutex);
        ABT_mutex_create(mutex_.get());
    }

    ~AbtMutexWrapper() {
        ABT_mutex_free(mutex_.get());
    }

    void lock() {
        ABT_mutex_lock(*mutex_);
    }

    void unlock() {
        ABT_mutex_unlock(*mutex_);
    }
};

