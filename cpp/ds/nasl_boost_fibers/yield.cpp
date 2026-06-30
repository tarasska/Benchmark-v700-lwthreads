#include <nasl/yield.hpp>

#include <boost/fiber/operations.hpp>

namespace nasl::core {

   void yield() {
      boost::this_fiber::yield();
   }

}