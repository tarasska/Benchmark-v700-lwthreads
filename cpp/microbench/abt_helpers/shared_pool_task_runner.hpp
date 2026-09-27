#pragma once

#include <cstddef>
#include <cstdlib>
#include <abt.h>
#include <functional>
#include <memory>
#include <iostream>

class SharedPoolTaskRunner {
  private:
    int xstream_cnt_;
    int thread_cnt_;
    int tasks_cnt_;
    std::unique_ptr<ABT_xstream[]> xstreams_;
    std::unique_ptr<ABT_pool[]> pools_;
    std::unique_ptr<ABT_sched[]> scheds_;
    std::unique_ptr<ABT_thread[]> threads_;
    std::unique_ptr<ABT_thread[]> tasks_;

  public:
    typedef void (*thread_routine)(void *);

    SharedPoolTaskRunner(int cores, int xstream_cnt, int thread_cnt) : SharedPoolTaskRunner(cores, xstream_cnt, thread_cnt, 0) {}

    SharedPoolTaskRunner(int cores, int xstream_cnt, int thread_cnt, int task_cnt) : xstream_cnt_(xstream_cnt), thread_cnt_(thread_cnt), tasks_cnt_(task_cnt)  {
        std::cout << "STREAMS = " << xstream_cnt << std::endl;
        xstreams_ = std::unique_ptr<ABT_xstream[]>(new ABT_xstream[xstream_cnt]);
        pools_ = std::unique_ptr<ABT_pool[]>(new ABT_pool[xstream_cnt]);
        scheds_ = std::unique_ptr<ABT_sched[]>(new ABT_sched[xstream_cnt]);
        threads_ = std::unique_ptr<ABT_thread[]>(new ABT_thread[thread_cnt]);
        if (tasks_cnt_ > 0) {
            tasks_ = std::unique_ptr<ABT_task[]>(new ABT_task[task_cnt]);
        } else {
            tasks_ = std::unique_ptr<ABT_task[]>{};
        }

        /* Initialize Argobots. */
        ABT_init(0, nullptr);

        /* Create pools. */
        for (int i = 0; i < xstream_cnt_; i++) {
            ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPMC, ABT_TRUE, &pools_[i]);
        }
        

        /* Create schedulers. */
        for (int i = 0; i < xstream_cnt_; i++) {
            auto tmp = std::unique_ptr<ABT_pool[]>(new ABT_pool[xstream_cnt_]);
            for (int j = 0; j < xstream_cnt_; j++) {
                tmp[j] = pools_[(i + j) % xstream_cnt_];
            }
            ABT_sched_create_basic(ABT_SCHED_DEFAULT, xstream_cnt_, tmp.get(),
                                       ABT_SCHED_CONFIG_NULL, &scheds_[i]);
        }

        /* Set up a primary execution stream. */
        ABT_xstream_self(&xstreams_[0]);
        ABT_xstream_set_main_sched(xstreams_[0], scheds_[0]);

        /* Create secondary execution streams. */
        for (int i = 1; i < xstream_cnt_; i++) {
            ABT_xstream_create(scheds_[i], &xstreams_[i]);
        }

        /* Pin streams to cores */
        for (int i = 0; i < xstream_cnt_; ++i) {
            ABT_xstream_set_cpubind(xstreams_[i], static_cast<int>(i % cores));
        }
    }

    [[nodiscard]] int get_max_tasks() const {
        return tasks_cnt_;
    }

    template<typename ThreadArgs>
    void run_threads(void (*thread_routine)(void *), std::vector<ThreadArgs>& thread_args) {
        for (int i = 0; i < thread_cnt_; i++) {
            int pool_id = i % xstream_cnt_;
            ABT_thread_create(pools_[pool_id], thread_routine, &thread_args[i],
                              ABT_THREAD_ATTR_NULL, &threads_[i]);
        }
    }

    template<typename ThreadArgPtr>
    void run_threads(void (*thread_routine)(void *), ThreadArgPtr* thread_args) {
        for (int i = 0; i < thread_cnt_; i++) {
            int pool_id = i % xstream_cnt_;
            ABT_thread_create(pools_[pool_id], thread_routine, thread_args[i],
                              ABT_THREAD_ATTR_NULL, &threads_[i]);
        }
    }

    template<typename TaskArgs>
    void run_tasks_blocking(void (*task_routine)(void *), TaskArgs* task_args) {
        for (int i = 0; i < tasks_cnt_; i++) {
            ABT_task_create(
                pools_[i % xstream_cnt_],
                task_routine,
                task_args,
                &tasks_[i]
            );
        }
        for (int i = 0; i < tasks_cnt_; i++) {
            ABT_task_free(&tasks_[i]);
        }
    }

    template<typename TaskArgs>
    void run_tasks_blocking(void (*task_routine)(void *), TaskArgs* task_args, int task_cnt) {
        for (int i = 0; i < std::min(task_cnt, tasks_cnt_); i++) {
            ABT_task_create(
                pools_[i % xstream_cnt_],
                task_routine,
                task_args,
                &tasks_[i]
            );
        }
        for (int i = 0; i < std::min(task_cnt, tasks_cnt_); i++) {
            ABT_task_free(&tasks_[i]); 
        }
    }

    void join_all_threads(std::function<void(int)> const& after_join_callback) {
        for (int i = 0; i < thread_cnt_; i++) {
            ABT_thread_free(&threads_[i]);
            after_join_callback(i);
        }
    }

    void join_all_threads() {
        for (int i = 0; i < thread_cnt_; i++) {
            ABT_thread_free(&threads_[i]);
        }
    }

    ~SharedPoolTaskRunner() {
        /* Join and free secondary execution streams. */
        for (int i = 1; i < xstream_cnt_; i++) {
            ABT_xstream_join(xstreams_[i]);
            ABT_xstream_free(&xstreams_[i]);
        }

        /* Finalize Argobots. */
        ABT_finalize();
    }
};
