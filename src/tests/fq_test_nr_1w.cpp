#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, true, false, false>;
// using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, false,
// true, false>;

int main(int argc, char **argv) {
    if (argc == 1) { println("usage : ./fq_test_nr_1w <buffer_size> <seed> <functions> <threads>"); }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 10000.0 / 1024.0 / 1024.0; }() * 1024 *
                                   1024;
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    println("total num_threads :", num_threads);

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 10);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    std::vector<size_t> result_vector;
    result_vector.reserve(functions);

    std::mutex result_mut;
    StartFlag startFlag;

    std::vector<std::jthread> reader_threads;
    for (auto t = num_threads; t--;) {
        reader_threads.emplace_back([&, str{"thread " + std::to_string(t + 1)}] {
            startFlag.wait();
            std::vector<size_t> res_vec;
            res_vec.reserve(11 * functions / num_threads / 10);
            {
                Timer timer{str};

                while (true) {
                    while (!rawComputeQueue.reserve()) std::this_thread::yield();
                    auto const res = rawComputeQueue.call_and_pop(seed);
                    if (res == std::numeric_limits<size_t>::max()) break;
                    else
                        res_vec.push_back(res);
                }
            }

            println("numbers computed : ", res_vec.size());
            std::cout << std::flush;

            std::lock_guard lock{result_mut};
            result_vector.insert(result_vector.end(), res_vec.begin(), res_vec.end());
        });
    }

    [=, &rawComputeQueue, &startFlag] {
        startFlag.start();
        auto func = functions;
        CallbackGenerator callbackGenerator{seed};
        while (func) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) {
                while (!rawComputeQueue.push_back(std::forward<T>(t))) { std::this_thread::yield(); }
                --func;
            });
        }

        for (auto t = num_threads; t--;)
            while (!rawComputeQueue.push_back([](auto) { return std::numeric_limits<size_t>::max(); }))
                ;
    }();

    reader_threads.clear();

    println("result vector size : ", result_vector.size());
    print("sorting result vector .... ");
    std::sort(result_vector.begin(), result_vector.end());
    println("result vector sorted");

    size_t hash = seed;
    for (auto r : result_vector) hash ^= r;
    println("result : ", hash);
}