#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, false, false>;
//using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, false, false, false>;

void test_lockFreeQueue(ComputeFunctionQueue &rawComputeQueue, CallbackGenerator &callbackGenerator,
                        size_t functions) noexcept;

int main(int argc, char **argv) {
    if (argc == 1) { println("usage : ./fq_test_1r_1w <buffer_size> <seed> <functions>"); }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 1000 / 1024.0 / 1024.0; }() * 1024 * 1024;

    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    CallbackGenerator callbackGenerator{seed};

    test_lockFreeQueue(rawComputeQueue, callbackGenerator, functions);
}

void test_lockFreeQueue(ComputeFunctionQueue &rawComputeQueue, CallbackGenerator &callbackGenerator,
                        size_t functions) noexcept {
    StartFlag start_flag;

    std::jthread reader{[&] {
        start_flag.wait();
        size_t num{0}, res{0};
        {
            Timer timer{"reader"};
            while (res != std::numeric_limits<size_t>::max()) {
                num = res;
                if (rawComputeQueue.reserve()) {
                    res = rawComputeQueue.call_and_pop(res);
                    //                    println(res);
                } else {
                    std::this_thread::yield();
                }
            }
        }
        println("result :", num, '\n');
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
