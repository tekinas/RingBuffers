#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <fmt/format.h>

#include <thread>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
using FunctionQueueSCSP = FunctionQueue_SCSP<ComputeFunctionSig, true, true, false>;
using FunctionQueueMCSP = FunctionQueue_MCSP<ComputeFunctionSig, true, false>;
using FunctionQueueType = FunctionQueueSCSP;

template<typename FunctionQueueType>
class FunctionQueueData {
private:
    std::unique_ptr<std::byte[]> functionQueueBuffer;
    [[no_unique_address]] std::conditional_t<std::same_as<FunctionQueueType, FunctionQueueMCSP>,
                                             std::unique_ptr<std::atomic<uint16_t>[]>, std::monostate>
            cleanOffsetArray;

public:
    FunctionQueueType functionQueue;

    template<typename = void>
    requires std::same_as<FunctionQueueType, FunctionQueueMCSP> FunctionQueueData(size_t buffer_size)
        : functionQueueBuffer{std::make_unique<std::byte[]>(buffer_size)},
          cleanOffsetArray{std::make_unique<std::atomic<uint16_t>[]>(FunctionQueueType::clean_array_size(buffer_size))},
          functionQueue{functionQueueBuffer.get(), buffer_size, cleanOffsetArray.get()} {}

    template<typename = void>
    requires std::same_as<FunctionQueueType, FunctionQueueSCSP> FunctionQueueData(size_t buffer_size)
        : functionQueueBuffer{std::make_unique<std::byte[]>(buffer_size)}, functionQueue{functionQueueBuffer.get(),
                                                                                         buffer_size} {}
};

template<typename FunctionQueue>
void reader(FunctionQueue &fq, size_t seed, uint16_t t, std::vector<size_t> &result_vector,
            std::mutex &res_vec_mut) noexcept {
    std::vector<size_t> res_vec;

    uint32_t func{0};

    if constexpr (std::same_as<FunctionQueue, FunctionQueueSCSP>) {
        while (fq.reserve()) {
            auto const res = fq.call_and_pop(seed);
            res_vec.push_back(res);
            ++func;
        }
    } else if (std::same_as<FunctionQueue, FunctionQueueMCSP>) {
        FunctionQueueMCSP::FunctionHandle handle;
        while ((handle = fq.get_function_handle())) {
            auto const res = handle.call_and_pop(seed);
            res_vec.push_back(res);
            ++func;
        }
    }

    fmt::print("thread {} read {} functions\n", t, func);

    std::lock_guard lock{res_vec_mut};
    result_vector.insert(result_vector.end(), res_vec.begin(), res_vec.end());
}

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_mr_mw <buffer_size> <seed> <writer_threads> <reader_threads>\n"); }

    size_t const functionQueueBufferSize = [&] { return (argc >= 2) ? atof(argv[1]) : 1024 + 150; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", functionQueueBufferSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);

    size_t const numWriterThreads = [&] { return (argc >= 4) ? atol(argv[3]) : std::thread::hardware_concurrency(); }();
    fmt::print("writer threads : {}\n", numWriterThreads);

    size_t const numReaderThreads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    fmt::print("reader threads : {}\n\n", numReaderThreads);

    FunctionQueueData<FunctionQueueType> fq_data{functionQueueBufferSize};

    {
        Timer timer{"data write time :"};
        std::vector<std::jthread> writer_threads;
        for (auto t = numWriterThreads; t--;) {
            writer_threads.emplace_back([=, &fq_data] {
                auto func = 3'100'000;
                CallbackGenerator callbackGenerator{seed};
                while (func) {
                    callbackGenerator.addCallback(util::overload(
                            [&]<typename T>(T &&t) {
                                while (!fq_data.functionQueue.push(std::forward<T>(t))) { std::this_thread::yield(); }
                                --func;
                            },
                            [&]<auto fp>() {
                                while (!fq_data.functionQueue.push<fp>()) { std::this_thread::yield(); }
                                --func;
                            }));
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
            reader_threads.emplace_back([&fq_data, t, seed, &result_vector, &res_vec_mut] {
                reader(fq_data.functionQueue, seed, t, result_vector, res_vec_mut);
            });
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
