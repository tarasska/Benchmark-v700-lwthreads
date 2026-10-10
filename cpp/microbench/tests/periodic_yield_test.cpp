#include "periodic_yield.h"
#include <cassert>
#include <iostream>

int main() {
    microbench::PeriodicYield<0> disabled;
    disabled.after_operation([] { assert(false); });

    microbench::PeriodicYield<1> every;
    int calls = 0;
    for (int i = 0; i < 10; ++i) every.after_operation([&] { ++calls; });
    assert(calls == 10);

    microbench::PeriodicYield<64> first, second;
    calls = 0;
    for (int i = 0; i < 63; ++i) first.after_operation([&] { ++calls; });
    second.after_operation([&] { ++calls; });
    assert(calls == 0); // Other logical worker has its own counter.
    first.after_operation([&] { ++calls; });
    assert(calls == 1);
    for (int i = 0; i < 64; ++i) first.after_operation([&] { ++calls; });
    assert(calls == 2);

    microbench::PeriodicYield<> configured;
    calls = 0;
    for (int i = 0; i < 256; ++i) configured.after_operation([&] { ++calls; });
    constexpr auto expected = BENCH_YIELD_EVERY == 0 ? 0 : 256 / BENCH_YIELD_EVERY;
    assert(calls == expected);
    std::cout << "Periodic yield tests passed, K=" << BENCH_YIELD_EVERY << '\n';
}
