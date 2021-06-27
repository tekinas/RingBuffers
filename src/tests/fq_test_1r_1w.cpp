#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <thread>

using ComputeFunctionSig = size_t(size_t);
//using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, false, false>;
using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, false, false>;

void test_lockFreeQueue(ComputeFunctionQueue &rawComputeQueue, CallbackGenerator &callbackGenerator,
                        size_t functions) noexcept;

using util::Timer;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_1r_1w <buffer_size> <seed> <functions>\n"); }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 1000 / 1024.0 / 1024.0; }() * 1024 * 1024;

    fmt::print("buffer size : {}\n", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    fmt::print("functions : {}\n", functions);

    auto const rawQueueMem = std::make_unique<std::byte[]>(rawQueueMemSize);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    CallbackGenerator callbackGenerator{seed};

    test_lockFreeQueue(rawComputeQueue, callbackGenerator, functions);
}

void test_lockFreeQueue(ComputeFunctionQueue &rawComputeQueue, CallbackGenerator &callbackGenerator,
                        size_t functions) noexcept {
    util::StartFlag start_flag;

    std::jthread reader{[&] {
        start_flag.wait();
        size_t num{0}, res{0};
        {
            Timer timer{"reader"};
            while (res != std::numeric_limits<size_t>::max()) {
                num = res;
                if (auto handle = rawComputeQueue.get_function_handle()) {
                    res = handle.call_and_pop(res);
                    //fmt::print("{}\n", res);
                } else {
                    std::this_thread::yield();
                }

                /*if (rawComputeQueue.reserve()) {
                    res = rawComputeQueue.call_and_pop(res);
                    //fmt::print("{}\n", res);
                } else {
                    std::this_thread::yield();
                }*/
            }
        }
        fmt::print("result : {}\n", num);
    }};

    std::jthread writer{[&] {
        start_flag.wait();
        auto func = functions;
        while (func) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) {
                while (!rawComputeQueue.push_back(std::forward<T>(t))) { std::this_thread::yield(); }
                --func;
            });
        }

        while (!rawComputeQueue.push_back([](auto) { return std::numeric_limits<size_t>::max(); }))
            ;
    }};

    start_flag.start();
}
