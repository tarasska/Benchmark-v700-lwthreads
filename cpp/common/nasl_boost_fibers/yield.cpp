#include <nasl/yield.hpp>

#include <boost/fiber/operations.hpp>

namespace nasl::core {

   void yield() {
#ifdef USE_COROUTINES     
      boost::this_fiber::yield();
#else
      std::this_thread::yield();      
#endif      
   }

}