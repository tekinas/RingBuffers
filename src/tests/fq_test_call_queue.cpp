#include "../FunctionQueue.h"
#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <deque>
#include <folly/Function.h>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue<ComputeFunctionSig, false>;
//using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, false, false>;
//using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, false, false, false>;

using folly::Function;
using util::Timer;

size_t *bytes_allocated = nullptr;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_call_and_pop <buffer_size> <seed>\n"); }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    std::deque<Function<ComputeFunctionSig>> computeDequeue;
    std::deque<std::function<ComputeFunctionSig>> computeStdDequeue;

    CallbackGenerator callbackGenerator{seed};

    auto const rawQueueMem = std::make_unique<std::byte[]>(rawQueueMemSize);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    size_t const compute_functors = [&] {
        Timer timer{"function queue fill time"};
        bool addFunction = true;
        while (addFunction) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { addFunction = rawComputeQueue.push_back(std::forward<T>(t)); });
        }

        return rawComputeQueue.size();
    }();

    size_t computeDequeueStorage{0}, computeStdDequeueStorage{0};

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of functions fill time"};
        bytes_allocated = &computeDequeueStorage;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeDequeue.emplace_back(std::forward<T>(t)); });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of std functions fill time"};
        bytes_allocated = &computeStdDequeueStorage;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { computeStdDequeue.emplace_back(std::forward<T>(t)); });
        }
    }

    fmt::print("\ncompute functions : {}\n", compute_functors);
    constexpr double ONE_MiB = 1024.0 * 1024.0;
    fmt::print("function queue storage : {} MiB\n", rawQueueMemSize / ONE_MiB);
    fmt::print("std::dequeue<folly::Function> storage : {} MiB\n", computeDequeueStorage / ONE_MiB);
    fmt::print("std::dequeue<std::function> storage : {} MiB\n\n", computeStdDequeueStorage / ONE_MiB);

    void test(ComputeFunctionQueue &) noexcept;
    void test(std::deque<Function<ComputeFunctionSig>> &) noexcept;
    void test(std::deque<std::function<ComputeFunctionSig>> &) noexcept;

    test(rawComputeQueue);
    test(computeDequeue);
    test(computeStdDequeue);
}

void test(ComputeFunctionQueue &rawComputeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"function queue"};
        while (rawComputeQueue.reserve()) { num = rawComputeQueue.call_and_pop(num); }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::deque<Function<ComputeFunctionSig>> &computeDequeue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of functions"};
        for (auto end = computeDequeue.end(), begin = computeDequeue.begin(); begin != end; ++begin) {
            num = (*begin)(num);
        }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::deque<std::function<ComputeFunctionSig>> &computeStdDequeue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of std functions"};
        for (auto end = computeStdDequeue.end(), begin = computeStdDequeue.begin(); begin != end; ++begin) {
            num = (*begin)(num);
        }
    }
    fmt::print("result : {}\n\n", num);
}

void *operator new(size_t bytes) {
    if (bytes_allocated) *bytes_allocated += bytes;
    return malloc(bytes);
}

void operator delete(void *ptr) { free(ptr); }
