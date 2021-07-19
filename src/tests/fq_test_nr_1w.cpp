#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"
#include <boost/container_hash/hash_fwd.hpp>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <thread>

using util::Timer;

using ComputeFunctionSig = size_t(size_t);
//using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, true, false, false>;
using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, false, false>;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_nr_1w <buffer_size> <seed> <functions> <threads>\n"); }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 1000.0 / 1024.0 / 1024.0; }() * 1024 *
                                   1024;
    fmt::print("buffer size : {}\n", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    fmt::print("functions : {}\n", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    fmt::print("reader threads : {}\n", num_threads);

    auto const rawQueueMem = std::make_unique<std::byte[]>(rawQueueMemSize);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    std::vector<size_t> result_vector;
    result_vector.reserve(functions);

    std::mutex result_mut;
    util::StartFlag startFlag;

    std::vector<std::jthread> reader_threads;
    for (auto t = num_threads; t--;) {
        reader_threads.emplace_back([&, str{fmt::format("thread {}", t + 1)}] {
            std::vector<size_t> res_vec;
            res_vec.reserve(11 * functions / num_threads / 10);

            startFlag.wait();
            {
                Timer timer{str};

                while (true) {
                    //while (!rawComputeQueue.reserve()) std::this_thread::yield();
                    //auto const res = rawComputeQueue.call_and_pop(seed);

                    ComputeFunctionQueue::FunctionHandle handle;
                    while (!(handle = rawComputeQueue.get_function_handle())) std::this_thread::yield();
                    auto const res = handle.call_and_pop(seed);

                    if (res == std::numeric_limits<size_t>::max()) break;
                    else
                        res_vec.push_back(res);
                }
            }

            fmt::print("numbers computed : {}\n", res_vec.size());

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

    fmt::print("result vector size : {}\n", result_vector.size());
    fmt::print("sorting result vector .... ");
    std::sort(result_vector.begin(), result_vector.end());
    fmt::print("result vector sorted\n");

    size_t hash = seed;
    boost::hash_range(hash, result_vector.cbegin(), result_vector.cend());
    fmt::print("result : {}\n", hash);
}
