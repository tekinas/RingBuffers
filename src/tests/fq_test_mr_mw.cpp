#include "ComputeCallbackGenerator.h"
#include "util.h"
#include <RingBuffers/FunctionQueue.h>
#include <RingBuffers/FunctionQueue_MCSP.h>
#include <RingBuffers/FunctionQueue_SCSP.h>

#include <fmt/format.h>

#include <mutex>
#include <thread>
#include <utility>

using namespace util;

using namespace rb;
using ComputeFunctionSig = size_t(size_t);
using FunctionQueueSCSP = FunctionQueue_SCSP<ComputeFunctionSig, true>;
using FunctionQueueMCSP = FunctionQueue_MCSP<ComputeFunctionSig, true>;
using FunctionQueueType = FunctionQueueMCSP;

template<typename FQType>
auto makeFunctionQueue(size_t buffer_size, uint16_t threads = 1) noexcept {
    if constexpr (std::same_as<FunctionQueueMCSP, FQType>) return FQType{static_cast<uint32_t>(buffer_size), threads};
    else
        return FQType{buffer_size};
}

template<typename FunctionQueue>
void reader(FunctionQueue &fq, size_t seed, uint16_t t, std::vector<size_t> &result_vector,
            std::mutex &res_vec_mut) noexcept {
    std::vector<size_t> res_vec;
    uint32_t func{0};
    if constexpr (std::same_as<FunctionQueue, FunctionQueueSCSP>) {
        static util::SpinLock read_lock;
        while (true) {
            std::scoped_lock lock{read_lock};
            if (fq.empty()) break;
            res_vec.push_back(fq.call_and_pop(seed));
            ++func;
        }
    } else if (std::same_as<FunctionQueue, FunctionQueueMCSP>) {
        auto reader = fq.get_reader(t);
        while (!fq.empty()) {
            if (auto handle = reader.get_function_handle(rb::check_once)) {
                auto const res = handle.call_and_pop(seed);
                res_vec.push_back(res);
                ++func;
            } else
                std::this_thread::yield();
        }
    }
    fmt::print("thread {} read {} functions\n", t, func);
    std::scoped_lock lock{res_vec_mut};
    result_vector.insert(result_vector.end(), res_vec.begin(), res_vec.end());
}

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_mr_mw <writer_threads> <reader_threads> <seed>\n"); }

    constexpr size_t buffer_size = (1024 + 150) * 1024 * 1024;

    auto const numWriterThreads =
            static_cast<uint16_t>([&] { return (argc >= 2) ? atol(argv[1]) : std::thread::hardware_concurrency(); }());
    fmt::print("writer threads : {}\n", numWriterThreads);

    size_t const numReaderThreads = [&] { return (argc >= 3) ? atol(argv[2]) : std::thread::hardware_concurrency(); }();
    fmt::print("reader threads : {}\n", numReaderThreads);

    size_t const seed = [&] { return (argc >= 4) ? atol(argv[3]) : std::random_device{}(); }();
    fmt::print("seed : {}\n\n", seed);

    auto functionQueue = makeFunctionQueue<FunctionQueueType>(buffer_size, numReaderThreads);

    {
        Timer timer{"data write time :"};
        std::vector<std::jthread> writer_threads;
        for (auto t = numWriterThreads; t--;) {
            writer_threads.emplace_back([=, &functionQueue] {
                static util::SpinLock write_lock;
                auto func = 3'100'000;
                CallbackGenerator callbackGenerator{seed};
                while (func) {
                    callbackGenerator.addCallback([&]<typename T>(T &&t) {
                        std::scoped_lock lock{write_lock};
                        functionQueue.push(std::forward<T>(t));
                        --func;
                    });
                }

                fmt::print("writer thread {} joined\n", t);
            });
        }
    }

    fmt::print("{} writer threads joined\n\n", numWriterThreads);

    std::vector<size_t> result_vector;
    std::mutex res_vec_mut;
    {
        Timer timer{"data read time :"};
        std::vector<std::jthread> reader_threads;
        for (auto t = numReaderThreads; t--;) {
            reader_threads.emplace_back([&functionQueue, t, seed, &result_vector, &res_vec_mut] {
                reader(functionQueue, seed, t, result_vector, res_vec_mut);
            });
        }
    }

    fmt::print("{} reader threads joined\n\n", numReaderThreads);

    fmt::print("result vector size : {}\n", result_vector.size());
    fmt::print("sorting result vector ....\n");
    std::sort(result_vector.begin(), result_vector.end());
    fmt::print("result vector sorted\n");

    size_t hash = seed;
    for (auto r : result_vector) boost::hash_combine(hash, r);
    fmt::print("result : {}\n", hash);
}
