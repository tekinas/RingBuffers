#include "FunctionQueue_SCSP.h"
#include "FunctionQueue_MCSP.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
//using LockFreeQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, true, false>;
using LockFreeQueue = FunctionQueue_MCSP<ComputeFunctionSig, true, false, false>;

int main(int argc, char **argv) {
    if (argc == 1) {
        println("usage : ./fq_test_nr_nw <buffer_size> <seed> <functions> <threads>");
    }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 10; }() * 1024 * 1024;
    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize);
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    println("total num_threads :", num_threads);

    LockFreeQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};
    StartFlag startFlag;

    std::vector<std::jthread> writer_threads;
    for (auto t = num_threads; t--;) {
        writer_threads.emplace_back([=, &rawComputeQueue, &startFlag] {
            startFlag.wait();
            auto func = functions;
            CallbackGenerator callbackGenerator{seed};
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
        });
    }

    std::vector<size_t> result_vector;
    result_vector.reserve(num_threads * functions);
    {
        startFlag.start();
        Timer timer{"reader thread "};
        auto threads = num_threads;

        while (threads) {
            while (!rawComputeQueue.reserve_function()) std::this_thread::yield();
            auto const res = rawComputeQueue.call_and_pop(seed);
//            std::cout << res << '\n';
            if (res == std::numeric_limits<size_t>::max()) --threads;
            else result_vector.push_back(res);
        }
    }

    writer_threads.clear();

    println("result vector size : ", result_vector.size());
    print("sorting result vector .... ");
    std::sort(result_vector.begin(), result_vector.end());
    println("result vector sorted");

    size_t hash = seed;
    for (auto r : result_vector)
        hash ^= r;
    println("result : ", hash);
}