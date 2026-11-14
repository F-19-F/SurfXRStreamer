#include "../surfxr_server/DeviceActivation.h"
#include <cassert>
#include <iostream>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    DeviceActivation delayed;
    assert(!delayed.ready());
    // An accepted registration survives an activation that has not started yet.
    assert(delayed.wait(0ms));
    assert(!delayed.ready());
    auto activator = std::thread([&] { delayed.complete(true); });
    assert(delayed.wait(1s));
    activator.join();
    assert(delayed.ready());

    delayed.deactivate();
    assert(!delayed.ready());
    assert(delayed.wait(0ms));
    delayed.complete(true);
    assert(delayed.ready());

    DeviceActivation failed;
    failed.complete(false);
    assert(!failed.wait(0ms));
    assert(!failed.ready());

    DeviceActivation asynchronous_failure;
    auto failure = std::thread([&] { asynchronous_failure.complete(false); });
    assert(!asynchronous_failure.wait(1s));
    failure.join();
    assert(!asynchronous_failure.ready());
    std::cout << "Device activation lifecycle checks passed\n";
}
