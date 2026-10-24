#pragma once

#include <abt.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

// All execution streams schedule ULTs from the same MPMC FIFO pool.
class SinglePoolTaskRunner {
  private:
    int xstream_cnt_;
    int thread_cnt_;
    int tasks_cnt_;
    ABT_pool pool_;
    std::unique_ptr<ABT_xstream[]> xstreams_;
    std::unique_ptr<ABT_sched[]> scheds_;
    std::unique_ptr<ABT_thread[]> threads_;
    std::unique_ptr<ABT_task[]> tasks_;

  public:
    typedef void (*thread_routine)(void *);

    SinglePoolTaskRunner(int cores, int xstream_cnt, int thread_cnt)
        : SinglePoolTaskRunner(cores, xstream_cnt, thread_cnt, 0) {}

    SinglePoolTaskRunner(int cores, int xstream_cnt, int thread_cnt, int task_cnt)
        : xstream_cnt_(xstream_cnt), thread_cnt_(thread_cnt), tasks_cnt_(task_cnt),
          xstreams_(new ABT_xstream[xstream_cnt]), scheds_(new ABT_sched[xstream_cnt]),
          threads_(new ABT_thread[thread_cnt]), tasks_(task_cnt > 0 ? new ABT_task[task_cnt] : nullptr) {
        std::cout << "STREAMS = " << xstream_cnt_ << std::endl;

        ABT_init(0, nullptr);
        ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPMC, ABT_TRUE, &pool_);

        for (int i = 0; i < xstream_cnt_; ++i) {
            ABT_sched_create_basic(ABT_SCHED_DEFAULT, 1, &pool_,
                                   ABT_SCHED_CONFIG_NULL, &scheds_[i]);
        }

        ABT_xstream_self(&xstreams_[0]);
        ABT_xstream_set_main_sched(xstreams_[0], scheds_[0]);
        for (int i = 1; i < xstream_cnt_; ++i) {
            ABT_xstream_create(scheds_[i], &xstreams_[i]);
        }
        for (int i = 0; i < xstream_cnt_; ++i) {
            ABT_xstream_set_cpubind(xstreams_[i], i % cores);
        }
    }

    [[nodiscard]] int get_max_tasks() const {
        return tasks_cnt_;
    }

    template<typename ThreadArgs>
    void run_threads(void (*routine)(void *), std::vector<ThreadArgs>& thread_args) {
        for (int i = 0; i < thread_cnt_; ++i) {
            ABT_thread_create(pool_, routine, &thread_args[i], ABT_THREAD_ATTR_NULL, &threads_[i]);
        }
    }

    template<typename ThreadArgPtr>
    void run_threads(void (*routine)(void *), ThreadArgPtr* thread_args) {
        for (int i = 0; i < thread_cnt_; ++i) {
            ABT_thread_create(pool_, routine, thread_args[i], ABT_THREAD_ATTR_NULL, &threads_[i]);
        }
    }

    template<typename TaskArgs>
    void run_tasks_blocking(void (*routine)(void *), TaskArgs* task_args) {
        run_tasks_blocking(routine, task_args, tasks_cnt_);
    }

    template<typename TaskArgs>
    void run_tasks_blocking(void (*routine)(void *), TaskArgs* task_args, int task_cnt) {
        const int count = std::min(task_cnt, tasks_cnt_);
        for (int i = 0; i < count; ++i) {
            ABT_task_create(pool_, routine, task_args, &tasks_[i]);
        }
        for (int i = 0; i < count; ++i) {
            ABT_task_free(&tasks_[i]);
        }
    }

    void join_all_threads(std::function<void(int)> const& after_join_callback) {
        for (int i = 0; i < thread_cnt_; ++i) {
            ABT_thread_free(&threads_[i]);
            after_join_callback(i);
        }
    }

    void join_all_threads() {
        for (int i = 0; i < thread_cnt_; ++i) {
            ABT_thread_free(&threads_[i]);
        }
    }

    ~SinglePoolTaskRunner() {
        for (int i = 1; i < xstream_cnt_; ++i) {
            ABT_xstream_join(xstreams_[i]);
            ABT_xstream_free(&xstreams_[i]);
        }
        ABT_finalize();
    }
};
