#pragma once

#include <nasl/suspendable_fwd.hpp>

namespace nasl::core { 

template<>
class Suspendable<nasl::core::DefaultSuspendData> {
  private:
    typedef nasl::core::DefaultSuspendData SuspendData;

  public:
    
    static void init(SuspendData* suspend_data) {
    }
    
    static bool suspend(SuspendData* suspend_data) {
        return false;
    }

    static void resume(SuspendData* suspend_data) {
    }
    
};

}