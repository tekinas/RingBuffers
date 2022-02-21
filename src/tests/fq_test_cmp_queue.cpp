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

using namespace rb;
using ComputeFunctionSig = size_t(size_t);
using FunctionQueueUnsync = FunctionQueue<ComputeFunctionSig, false>;
using FunctionQueueSCSP = FunctionQueue_SCSP<ComputeFunctionSig, false>;
using FunctionQueueMCSP = FunctionQueue_MCSP<ComputeFunctionSig, false>;
using FunctionQueueType = FunctionQueueMCSP;

template<typename FQType>
auto makeFunctionQueue(size_t buffer_size, uint16_t threads = 1) noexcept {
    if constexpr (std::same_as<FunctionQueueMCSP, FQType>) return FQType{static_cast<uint32_t>(buffer_size), threads};
    else
        return FQType{buffer_size};
}

using util::Timer;

size_t default_alloc{};
size_t *bytes_allocated = &default_alloc;

template<typename FunctionQueueType>
requires std::same_as<FunctionQueueType, FunctionQueueUnsync> || std::same_as<FunctionQueueType, FunctionQueueSCSP> ||
        std::same_as<FunctionQueueType, FunctionQueueMCSP>
void test(FunctionQueueType &functionQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"function queue"};
        if constexpr (std::same_as<FunctionQueueType, FunctionQueueUnsync> ||
                      std::same_as<FunctionQueueType, FunctionQueueSCSP>)
            while (!functionQueue.empty()) num = functionQueue.call_and_pop(num);
        else {
            auto reader = functionQueue.get_reader(0);
            while (!functionQueue.empty()) num = reader.get_function_handle(rb::check_once).call_and_pop(num);
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

    size_t const buffer_size = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", buffer_size);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);


    auto functionQueue = makeFunctionQueue<FunctionQueueType>(buffer_size);

    CallbackGenerator callbackGenerator{seed};
    size_t const compute_functors = [&] {
        Timer timer{"function queue write time"};

        uint32_t functions{0};
        bool addFunction = true;
        while (addFunction) {
            ++functions;
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { addFunction = functionQueue.push(std::forward<T>(t)); });
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
    fmt::print("function queue memory footprint : {} MiB\n\n", functionQueue.buffer_size() / ONE_MiB);

    test(computeQueue);
    test(computeStdQueue);
    test(functionQueue);
}


void *operator new(size_t bytes) {
    *bytes_allocated += bytes;
    return malloc(bytes);
}

void operator delete(void *ptr, size_t) noexcept { free(ptr); }
void operator delete(void *ptr) noexcept { free(ptr); }
