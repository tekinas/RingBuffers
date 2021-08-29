#include "ComputeCallbackGenerator.h"
#include "util.h"
#include <RingBuffers/FunctionQueue.h>
#include <RingBuffers/FunctionQueue_MCSP.h>
#include <RingBuffers/FunctionQueue_SCSP.h>

#include <bit>
#include <boost/circular_buffer.hpp>
#include <chrono>
#include <folly/Function.h>
#include <random>
#include <type_traits>

#include <fmt/format.h>

using ComputeFunctionSig = size_t(size_t);
using FunctionQueueNC = FunctionQueue<ComputeFunctionSig, false>;
using FunctionQueueSCSP = FunctionQueue_SCSP<ComputeFunctionSig, false>;
using FunctionQueueMCSP = FunctionQueue_MCSP<ComputeFunctionSig, false>;
using FunctionQueueType = FunctionQueueMCSP;

using util::Timer;

size_t default_alloc{};
size_t *bytes_allocated = &default_alloc;

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
    requires std::same_as<FunctionQueueType, FunctionQueueNC> || std::same_as<FunctionQueueType, FunctionQueueSCSP>
    FunctionQueueData(size_t buffer_size)
        : functionQueueBuffer{std::make_unique<std::byte[]>(buffer_size)}, functionQueue{functionQueueBuffer.get(),
                                                                                         buffer_size} {}
};

template<typename FunctionQueueType>
requires std::same_as<FunctionQueueType, FunctionQueueNC> || std::same_as<FunctionQueueType, FunctionQueueSCSP> ||
        std::same_as<FunctionQueueType, FunctionQueueMCSP>
void test(FunctionQueueType &functionQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"function queue"};
        if constexpr (std::same_as<FunctionQueueType, FunctionQueueNC> ||
                      std::same_as<FunctionQueueType, FunctionQueueSCSP>)
            while (!functionQueue.empty()) num = functionQueue.call_and_pop(num);
        else {
            FunctionQueueMCSP::FunctionHandle handle;
            while ((handle = functionQueue.get_function_handle_check_once())) num = handle.call_and_pop(num);
        }
    }

    fmt::print("result : {}\n\n", num);
}

void test(boost::circular_buffer<folly::Function<ComputeFunctionSig>> &computeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"boost::circular_buffer<folly::Function>"};
        while (!computeQueue.empty()) {
            num = computeQueue.front()(num);
            computeQueue.pop_front();
        }
    }
    fmt::print("result : {}\n\n", num);
}

void test(boost::circular_buffer<std::function<ComputeFunctionSig>> &computeStdQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"boost::circular_buffer<std::function>"};
        while (!computeStdQueue.empty()) {
            num = computeStdQueue.front()(num);
            computeStdQueue.pop_front();
        }
    }
    fmt::print("result : {}\n\n", num);
}

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_call_and_pop <buffer_size> <seed>\n"); }

    size_t const functionQueueBufferSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", functionQueueBufferSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);


    FunctionQueueData<FunctionQueueType> fq_data{functionQueueBufferSize};

    CallbackGenerator callbackGenerator{seed};
    size_t const compute_functors = [&] {
        Timer timer{"function queue write time"};

        uint32_t functions{0};
        bool addFunction = true;
        while (addFunction) {
            ++functions;
            callbackGenerator.addCallback(util::overload(
                    [&]<typename T>(T &&t) { addFunction = fq_data.functionQueue.push(std::forward<T>(t)); },
                    [&]<auto fp> { addFunction = fq_data.functionQueue.push<fp>(); }));
        }
        --functions;

        return functions;
    }();


    boost::circular_buffer<folly::Function<ComputeFunctionSig>> computeQueue{compute_functors};
    boost::circular_buffer<std::function<ComputeFunctionSig>> computeStdQueue{compute_functors};

    size_t follyFunctionAllocs{0}, stdFunctionAllocs{0};
    callbackGenerator.setSeed(seed);
    {
        Timer timer{"boost::circular_buffer<folly::Functions> write time"};
        bytes_allocated = &follyFunctionAllocs;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeQueue.push_back(std::forward<T>(t)); });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"boost::circular_buffer<std::functions> write time"};
        bytes_allocated = &stdFunctionAllocs;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeStdQueue.push_back(std::forward<T>(t)); });
        }
    }

    fmt::print("\ncompute functions : {}\n\n", compute_functors);

    constexpr double ONE_MiB = 1024.0 * 1024.0;
    fmt::print("boost::circular_buffer<folly::Function> memory footprint : {} MiB\n",
               (follyFunctionAllocs + computeQueue.size() * sizeof(folly::Function<ComputeFunctionSig>)) / ONE_MiB);
    fmt::print("boost::circular_buffer<std::function> memory footprint : {} MiB\n",
               (stdFunctionAllocs + computeStdQueue.size() * sizeof(std::function<ComputeFunctionSig>)) / ONE_MiB);
    fmt::print("function queue memory footprint : {} MiB\n\n", fq_data.functionQueue.buffer_size() / ONE_MiB);

    test(computeQueue);
    test(computeStdQueue);
    test(fq_data.functionQueue);
}


void *operator new(size_t bytes) {
    *bytes_allocated += bytes;
    return malloc(bytes);
}

void operator delete(void *ptr, size_t) { free(ptr); }
void operator delete(void *ptr) { free(ptr); }
