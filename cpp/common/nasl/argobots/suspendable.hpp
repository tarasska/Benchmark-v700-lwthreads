#pragma once

#include <nasl/suspendable_fwd.hpp>
#include <abt.h>

namespace nasl::core { 

template<>
class Suspendable<nasl::core::DefaultSuspendData> {
  private:
    typedef nasl::core::DefaultSuspendData SuspendData;

    static inline std::uintptr_t GetAbtSelfThread() {
        ABT_thread abt_thread;
        ABT_thread_self(&abt_thread);
        return reinterpret_cast<std::uintptr_t>(abt_thread);
    }
  
  public:
    
    static void init(SuspendData* suspend_data) {
        suspend_data->state_ptr.store(SuspendData::kReadyForSuspend, std::memory_order_release);
    }
    
    static bool suspend(SuspendData* suspend_data) {
        auto expected_state = SuspendData::kReadyForSuspend;
        if (suspend_data->state_ptr.compare_exchange_strong(expected_state, GetAbtSelfThread(), 
                                                                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
            ABT_self_suspend();
            return true;
        } else {
            return false;
        }
    }

    static void resume(SuspendData* suspend_data) {
        if (suspend_data == nullptr) {
            return;
        }
    
        auto expected_state = SuspendData::kReadyForSuspend;
        if (suspend_data->state_ptr.compare_exchange_strong(expected_state, SuspendData::kKeepActive,
                                                                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return;
        }
    
        if (expected_state > SuspendData::kKeepActive) {
            auto next_thread = reinterpret_cast<ABT_thread>(suspend_data->state_ptr.exchange(SuspendData::kKeepActive, std::memory_order_seq_cst));
            ABT_thread_state state = ABT_THREAD_STATE_READY;
            // waiting for suspend
            while (state != ABT_THREAD_STATE_BLOCKED) {
                ABT_thread_get_state(next_thread, &state);
            }
            ABT_thread_resume(next_thread);
        }
    }
    
};

}