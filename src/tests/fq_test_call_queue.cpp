#include "../FunctionQueue.h"
#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <queue>
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

    size_t const functionBufferSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", functionBufferSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    std::queue<Function<ComputeFunctionSig>> computeDequeue;
    std::queue<std::function<ComputeFunctionSig>> computeStdDequeue;

    CallbackGenerator callbackGenerator{seed};

    auto const functionBuffer = std::make_unique<std::byte[]>(functionBufferSize);
    ComputeFunctionQueue functionQueue{functionBuffer.get(), functionBufferSize};

    size_t computeDequeueStorage{0}, computeStdDequeueStorage{0};

    size_t const compute_functors = [&] {
        Timer timer{"function queue write time"};

        uint32_t functions{0};
        bool addFunction = true;
        while (addFunction) {
            ++functions;
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { addFunction = functionQueue.push_back(std::forward<T>(t)); });
        }
        --functions;

        return functions;
    }();

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of functions fill time"};
        bytes_allocated = &computeDequeueStorage;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeDequeue.emplace(std::forward<T>(t)); });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of std functions fill time"};
        bytes_allocated = &computeStdDequeueStorage;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeStdDequeue.emplace(std::forward<T>(t)); });
        }
    }

    fmt::print("\ncompute functions : {}\n", compute_functors);
    constexpr double ONE_MiB = 1024.0 * 1024.0;
    fmt::print("function queue memory : {} MiB\n", functionBufferSize / ONE_MiB);
    fmt::print("std::dequeue<folly::Function> memory : {} MiB\n", computeDequeueStorage / ONE_MiB);
    fmt::print("std::dequeue<std::function> memory : {} MiB\n\n", computeStdDequeueStorage / ONE_MiB);

    void test(ComputeFunctionQueue &) noexcept;
    void test(std::queue<Function<ComputeFunctionSig>> &) noexcept;
    void test(std::queue<std::function<ComputeFunctionSig>> &) noexcept;

    test(functionQueue);
    test(computeDequeue);
    test(computeStdDequeue);
}

void test(ComputeFunctionQueue &functionQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"function queue"};
        while (!functionQueue.empty()) { num = functionQueue.call_and_pop(num); }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::queue<Function<ComputeFunctionSig>> &computeDequeue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of functions"};
        while (!computeDequeue.empty()) {
            num = computeDequeue.front()(num);
            computeDequeue.pop();
        }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::queue<std::function<ComputeFunctionSig>> &computeStdDequeue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of std functions"};
        while (!computeStdDequeue.empty()) {
            num = computeStdDequeue.front()(num);
            computeStdDequeue.pop();
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
