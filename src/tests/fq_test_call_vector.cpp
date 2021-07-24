#include "../FunctionQueue.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <chrono>
#include <folly/Function.h>

#define FMT_HEADER_ONLY
#include <fmt/format.h>


using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue<ComputeFunctionSig, false>;

using folly::Function;
using util::Timer;

size_t *bytes_allocated = nullptr;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_call_only <buffer_size> <seed>\n"); }

    size_t const functionQueueBufferSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", functionQueueBufferSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    std::vector<Function<ComputeFunctionSig>> computeVector{};
    std::vector<std::function<ComputeFunctionSig>> computeStdVector{};

    CallbackGenerator callbackGenerator{seed};

    auto const functionQueueBuffer = std::make_unique<std::byte[]>(functionQueueBufferSize);
    ComputeFunctionQueue functionQueue{functionQueueBuffer.get(), functionQueueBufferSize};

    size_t const compute_functors = [&] {
        Timer timer{"function queue write time"};

        uint32_t functions{0};
        bool addFunction = true;
        while (addFunction) {
            ++functions;
            callbackGenerator.addCallback(util::overload(
                    [&]<typename T>(T &&t) { addFunction = functionQueue.push_back(std::forward<T>(t)); },
                    [&]<ComputeFunctionSig * fp> { addFunction = functionQueue.push_back<fp>(); }));
        }
        --functions;

        return functions;
    }();

    size_t computeVectorStorage{0}, computeStdVectorStorage{0};

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"vector of functions write time"};
        bytes_allocated = &computeVectorStorage;
        computeVector.reserve(compute_functors);

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeVector.emplace_back(std::forward<T>(t)); });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"vector of std functions write time"};
        bytes_allocated = &computeStdVectorStorage;
        computeStdVector.reserve(compute_functors);

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { computeStdVector.emplace_back(std::forward<T>(t)); });
        }
    }

    fmt::print("\ncompute functions : {}\n", compute_functors);

    constexpr double ONE_MiB = 1024.0 * 1024.0;
    fmt::print("function queue memory : {} MiB\n", functionQueueBufferSize / ONE_MiB);
    fmt::print("std::vector<folly::Function> memory : {} MiB\n", computeVectorStorage / ONE_MiB);
    fmt::print("std::vector<std::function> memory : {} MiB\n\n", computeStdVectorStorage / ONE_MiB);

    void test(ComputeFunctionQueue &) noexcept;
    void test(std::vector<Function<ComputeFunctionSig>> &) noexcept;
    void test(std::vector<std::function<ComputeFunctionSig>> &) noexcept;

    test(functionQueue);
    test(computeVector);
    test(computeStdVector);
}

void test(ComputeFunctionQueue &functionQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"function queue"};
        while (!functionQueue.empty()) { num = functionQueue.call_and_pop(num); }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::vector<Function<ComputeFunctionSig>> &vectorComputeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::vector of folly::Function"};

        for (auto &function : vectorComputeQueue) {
            num = function(num);
            //function = {};
        }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::vector<std::function<ComputeFunctionSig>> &vectorStdComputeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::vector of std function"};

        for (auto &function : vectorStdComputeQueue) {
            num = function(num);
            //function = {};
        }
    }
    fmt::print("result : {}\n\n", num);
}

void *operator new(size_t bytes) {
    if (bytes_allocated) *bytes_allocated += bytes;
    return malloc(bytes);
}

void operator delete(void *ptr, size_t) { free(ptr); }
void operator delete(void *ptr) { free(ptr); }
