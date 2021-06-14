#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
// using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, true,
// true, false>;
using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, true, false, false>;

int main(int argc, char **argv) {
    if (argc == 1) {
        println("usage : ./fq_test_mr_mw <buffer_size> <seed> <functions> "
                "<writer_threads> <reader_threads>");
    }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 1024 + 150; }() * 1024 * 1024;
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const numWriterThreads = [&] { return (argc >= 4) ? atol(argv[3]) : std::thread::hardware_concurrency(); }();
    println("total writer threads :", numWriterThreads);

    size_t const numReaderThreads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    println("total reader threads :", numReaderThreads);
    println();

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 20);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    {/// writing functions to the queue concurrently ::::
        Timer timer{"data write time :"};

        {
            std::vector<std::jthread> writer_threads;
            for (auto t = numWriterThreads; t--;) {
                writer_threads.emplace_back([=, &rawComputeQueue] {
                    auto func = 3'100'000;
                    CallbackGenerator callbackGenerator{seed};
                    while (func) {
                        callbackGenerator.addCallback([&]<typename T>(T &&t) {
                            while (!rawComputeQueue.push_back(std::forward<T>(t))) { std::this_thread::yield(); }
                            --func;
                        });
                    }

                    println("thread ", t, " finished");
                });
            }
        }
    }
    println(numWriterThreads, " writer threads joined\n");

    size_t const functions = rawComputeQueue.size();
    std::vector<size_t> result_vector;
    result_vector.reserve(functions);

    std::mutex res_vec_mut;

    {/// reading functions from the queue concurrently ::::
        Timer timer{"data read time :"};

        {
            std::vector<std::jthread> reader_threads;
            for (auto t = numReaderThreads; t--;) {
                reader_threads.emplace_back([=, &rawComputeQueue, &result_vector, &res_vec_mut] {
                    std::vector<size_t> res_vec;
                    res_vec.reserve(11 * functions / numReaderThreads / 10);

                    uint32_t func{0};
                    while (rawComputeQueue.reserve()) {
                        auto const res = rawComputeQueue.call_and_pop(seed);
                        res_vec.push_back(res);
                        ++func;
                    }

                    println("thread ", t, " read ", func, " functions");

                    std::lock_guard lock{res_vec_mut};
                    result_vector.insert(result_vector.end(), res_vec.begin(), res_vec.end());
                });
            }
        }
    }
    println(numReaderThreads, " reader threads joined\n");

    println("result vector size : ", result_vector.size());
    print("sorting result vector .... ");
    std::sort(result_vector.begin(), result_vector.end());
    println("result vector sorted");

    size_t hash = seed;
    for (auto r : result_vector) hash ^= r + r * hash;
    println("result : ", hash);
}
