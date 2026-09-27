#include <nasl/yield.hpp>

#include <abt.h>

namespace nasl::core {

   void yield() {
      ABT_thread_yield();    
   }

}