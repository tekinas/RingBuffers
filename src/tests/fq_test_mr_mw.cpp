#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, true, true, false>;
//using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, true, false>;

int main(int argc, char **argv) {
    if (argc == 1) {
        fmt::print("usage : ./fq_test_mr_mw <buffer_size> <seed> <functions> "
                   "<writer_threads> <reader_threads>\n");
    }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 1024 + 150; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    size_t const numWriterThreads = [&] { return (argc >= 4) ? atol(argv[3]) : std::thread::hardware_concurrency(); }();
    fmt::print("writer threads : {}\n", numWriterThreads);

    size_t const numReaderThreads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    fmt::print("reader threads : {}\n\n", numReaderThreads);

    auto const rawQueueMem = std::make_unique<std::byte[]>(rawQueueMemSize + 20);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    {/// adding functions to the queue exclusively using internal lock ::::
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

                    fmt::print("writer thread {} joined\n", t);
                });
            }
        }
    }
    fmt::print("{} writer threads joined\n\n", numWriterThreads);

    std::vector<size_t> result_vector;
    std::mutex res_vec_mut;
    {/// reading functions from the queue concurrently in case of MCSP queue and exclusively in case of SCSP queue ::::
        Timer timer{"data read time :"};

        {
            std::vector<std::jthread> reader_threads;
            for (auto t = numReaderThreads; t--;) {
                reader_threads.emplace_back([=, &rawComputeQueue, &result_vector, &res_vec_mut] {
                    std::vector<size_t> res_vec;

                    uint32_t func{0};

                    while (rawComputeQueue.reserve()) {
                        auto const res = rawComputeQueue.call_and_pop(seed);
                        res_vec.push_back(res);
                        ++func;
                    }

                    /*ComputeFunctionQueue::FunctionHandle handle;
                    while ((handle = rawComputeQueue.get_function_handle())) {
                        auto const res = handle.call_and_pop(seed);
                        res_vec.push_back(res);
                        ++func;
                    }*/

                    fmt::print("thread {} read {} functions\n", t, func);

                    std::lock_guard lock{res_vec_mut};
                    result_vector.insert(result_vector.end(), res_vec.begin(), res_vec.end());
                });
            }
        }
    }
    fmt::print("{} reader threads joined\n\n", numReaderThreads);

    fmt::print("result vector size : {}\n", result_vector.size());
    fmt::print("sorting result vector .... ");
    std::sort(result_vector.begin(), result_vector.end());
    fmt::print("result vector sorted\n");

    size_t hash = seed;
    for (auto r : result_vector) boost::hash_combine(hash, r);
    fmt::print("result : {}\n", hash);
}
