#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <fmt/format.h>

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
//using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, true, false>;
using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, true, false>;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_1r_nw <buffer_size> <seed> <functions> <threads>\n"); }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 10; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    fmt::print("functions : {}\n", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    fmt::print("writer threads : {}\n", num_threads);

    auto const rawQueueMem = std::make_unique<std::byte[]>(rawQueueMemSize);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};
    StartFlag startFlag;

    std::vector<std::jthread> writer_threads;
    for (auto t = num_threads; t--;) {
        writer_threads.emplace_back([=, &rawComputeQueue, &startFlag] {
            startFlag.wait();
            auto func = functions;
            CallbackGenerator callbackGenerator{seed};
            while (func) {
                callbackGenerator.addCallback([&]<typename T>(T &&t) {
                    while (!rawComputeQueue.push_back(std::forward<T>(t))) { std::this_thread::yield(); }
                    --func;
                });
            }

            while (!rawComputeQueue.push_back([](auto) noexcept { return std::numeric_limits<size_t>::max(); }))
                ;
        });
    }

    std::vector<size_t> result_vector;
    result_vector.reserve(num_threads * functions);
    {
        startFlag.start();
        Timer timer{"reader thread "};
        auto threads = num_threads;

        while (threads) {
            ComputeFunctionQueue::FunctionHandle handle;
            while (!(handle = rawComputeQueue.get_function_handle())) std::this_thread::yield();
            auto const res = handle.call_and_pop(seed);

            if (res == std::numeric_limits<size_t>::max()) --threads;
            else
                result_vector.push_back(res);
        }
    }

    writer_threads.clear();

    fmt::print("result vector size : {}\n", result_vector.size());
    fmt::print("sorting result vector ....\n");
    std::sort(result_vector.begin(), result_vector.end());
    fmt::print("result vector sorted\n");

    size_t hash = seed;
    for (auto r : result_vector) boost::hash_combine(hash, r);
    fmt::print("result : {}\n", hash);
}
