#pragma once

#include <abt.h>

namespace nasl::benchmark::abt_threads {

class AbtBarrier {
  private:
    ABT_barrier barrier_;
  public:
    explicit AbtBarrier(size_t waiters_cnt) {
        ABT_barrier_create(waiters_cnt, &barrier_);
    }

    void wait() {
        ABT_barrier_wait(barrier_);
    }

    ~AbtBarrier() {
        ABT_barrier_free(&barrier_);
    }
};

}

