#include "FunctionQueue_SCSP.h"
#include "FunctionQueue.h"
#include "FunctionQueue_MCSP.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
//using LockFreeQueue = FunctionQueue<true, true, ComputeFunctionSig>;
//using LockFreeQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, false>;
using LockFreeQueue = FunctionQueue_MCSP<ComputeFunctionSig, false, false>;


void test_lockFreeQueue(LockFreeQueue &rawComputeQueue, CallbackGenerator &callbackGenerator, size_t functions);

int main(int argc, char **argv) {
    size_t const rawQueueMemSize =
            [&] { return (argc >= 2) ? atof(argv[1]) : 500 / 1024.0 / 1024.0; }() * 1024 * 1024;

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 100);
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    LockFreeQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    CallbackGenerator callbackGenerator{seed};

    test_lockFreeQueue(rawComputeQueue, callbackGenerator, functions);
}

void test_lockFreeQueue(LockFreeQueue &rawComputeQueue, CallbackGenerator &callbackGenerator, size_t functions) {
    std::thread reader{[&] {
        size_t num{0}, res{0};
        {
            Timer timer{"reader"};
            while (res != std::numeric_limits<size_t>::max()) {
                num = res;
                if (rawComputeQueue) {
                    res = rawComputeQueue.callAndPop(res);
//                    println(res);
                } else {
                    std::this_thread::yield();
                }
            }
        }
        println("result :", num, '\n');
    }};

    std::thread writer{[&] {
        auto func = functions;
        while (func) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) {
                        while (!rawComputeQueue.push_back(std::forward<T>(t))) {
                            std::this_thread::yield();
                        }
                        --func;
                    });
        }

        while (!rawComputeQueue.push_back([](auto) { return std::numeric_limits<size_t>::max(); }));
    }};

    writer.join();
    reader.join();
}
