#pragma once

#include <nasl/suspendable_fwd.hpp>
#include <boost/fiber/all.hpp>

namespace nasl::core { 

template<>
class Suspendable<nasl::core::DefaultSuspendData> {
  private:
    typedef nasl::core::DefaultSuspendData SuspendData;
  
  public:

    static void init(SuspendData* suspend_data, std::memory_order mo = std::memory_order_seq_cst) {
        suspend_data->state_ptr.store(SuspendData::kReadyForSuspend, mo);
    }
    
    static bool suspend(SuspendData* suspend_data) {
        boost::fibers::promise<bool> resume_promise;
        std::uintptr_t resume_event_ptr = reinterpret_cast<std::uintptr_t>(&resume_promise);
        auto expected_state = SuspendData::kReadyForSuspend;
        if (suspend_data->state_ptr.compare_exchange_strong(expected_state, resume_event_ptr)) {
            resume_promise.get_future().wait();
            return true;
        }
            
        return false;
    }

    static void resume(SuspendData* suspend_data) {
        if (suspend_data == nullptr) {
            return;
        }
        std::uintptr_t resume_event_ptr = suspend_data->state_ptr.exchange(SuspendData::kKeepActive);
        if (resume_event_ptr > SuspendData::kKeepActive) {
            auto* casted_resume_event_ptr = reinterpret_cast<boost::fibers::promise<bool>*>(resume_event_ptr);
            suspend_data->state_ptr.store(SuspendData::kReadyForSuspend);
            casted_resume_event_ptr->set_value(true);
        }
    }
};

}