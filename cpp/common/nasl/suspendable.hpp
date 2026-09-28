#ifdef USE_BOOST_FIBERS
#include "./boost_fibers/suspendable.hpp"
#endif

#ifdef USE_ARGOBOTS
#include "./argobots/suspendable.hpp"
#endif

#ifdef USE_OS
#include "./os/suspendable.hpp"
#endif