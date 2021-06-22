#include "../FunctionQueue.h"
#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <chrono>
#include <folly/Function.h>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue<ComputeFunctionSig, false>;
//using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, false, false>;
// using ComputeFunctionQueue =  FunctionQueue_MCSP<ComputeFunctionSig, false, false, false>;

using folly::Function;
size_t *bytes_allocated = nullptr;

int main(int argc, char **argv) {
    if (argc == 1) { println("usage : ./fq_test_call_only <buffer_size> <seed>"); }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    std::vector<Function<ComputeFunctionSig>> computeVector{};
    std::vector<std::function<ComputeFunctionSig>> computeStdVector{};

    CallbackGenerator callbackGenerator{seed};

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    size_t const compute_functors = [&] {
        uint32_t functions{0};
        Timer timer{"function queue write time"};
        bool addFunction = true;
        while (addFunction) {
            ++functions;
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { addFunction = rawComputeQueue.push_back(std::forward<T>(t)); });
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

    println();
    println("total compute functions : ", compute_functors);
    constexpr double ONE_MB = 1024.0 * 1024.0;
    println("function queue storage :", rawQueueMemSize / ONE_MB, " Mb");
    println("std::vector<folly::Function> storage :", computeVectorStorage / ONE_MB, " Mb");
    println("std::vector<std::function> storage :", computeStdVectorStorage / ONE_MB, " Mb");

    println();

    void test(ComputeFunctionQueue &) noexcept;
    void test(std::vector<Function<ComputeFunctionSig>> &) noexcept;
    void test(std::vector<std::function<ComputeFunctionSig>> &) noexcept;

    test(rawComputeQueue);
    test(computeVector);
    test(computeStdVector);
}

void test(ComputeFunctionQueue &rawComputeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"function queue"};
        while (rawComputeQueue.reserve()) { num = rawComputeQueue.call_and_pop(num); }
    }
    println("result :", num, '\n');
}

void test(std::vector<Function<ComputeFunctionSig>> &vectorComputeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::vector of functions"};

        for (auto &&function : vectorComputeQueue) { num = function(num); }
    }
    println("result :", num, '\n');
}

void test(std::vector<std::function<ComputeFunctionSig>> &vectorStdComputeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::vector of std functions"};

        for (auto &&function : vectorStdComputeQueue) { num = function(num); }
    }
    println("result :", num, '\n');
}

void *operator new(size_t bytes) {
    if (bytes_allocated) *bytes_allocated += bytes;
    return malloc(bytes);
}

void operator delete(void *ptr) { free(ptr); }
