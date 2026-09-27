#include <nasl/yield.hpp>

#include <thread>

namespace nasl::core {

   void yield() {
      std::this_thread::yield();           
   }

}