#include "../FunctionQueue.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <folly/Function.h>
#include <queue>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue<ComputeFunctionSig, false>;

using folly::Function;
using util::Timer;

size_t *bytes_allocated = nullptr;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_call_and_pop <buffer_size> <seed>\n"); }

    size_t const functionQueueBufferSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", functionQueueBufferSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);

    std::queue<Function<ComputeFunctionSig>> computeQueue;
    std::queue<std::function<ComputeFunctionSig>> computeStdQueue;

    CallbackGenerator callbackGenerator{seed};

    auto const functionQueueBuffer =
            std::make_unique<std::aligned_storage_t<ComputeFunctionQueue::BUFFER_ALIGNMENT, sizeof(std::byte)>[]>(
                    functionQueueBufferSize);
    ComputeFunctionQueue functionQueue{std::bit_cast<std::byte *>(functionQueueBuffer.get()), functionQueueBufferSize};

    size_t computeDequeueStorage{0}, computeStdDequeueStorage{0};

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

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of functions fill time"};
        bytes_allocated = &computeDequeueStorage;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeQueue.emplace(std::forward<T>(t)); });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of std functions fill time"};
        bytes_allocated = &computeStdDequeueStorage;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeStdQueue.emplace(std::forward<T>(t)); });
        }
    }

    fmt::print("\ncompute functions : {}\n", compute_functors);
    constexpr double ONE_MiB = 1024.0 * 1024.0;
    fmt::print("function queue memory : {} MiB\n", functionQueueBufferSize / ONE_MiB);
    fmt::print("std::dequeue<folly::Function> memory : {} MiB\n", computeDequeueStorage / ONE_MiB);
    fmt::print("std::dequeue<std::function> memory : {} MiB\n\n", computeStdDequeueStorage / ONE_MiB);

    void test(ComputeFunctionQueue &) noexcept;
    void test(std::queue<Function<ComputeFunctionSig>> &) noexcept;
    void test(std::queue<std::function<ComputeFunctionSig>> &) noexcept;

    test(functionQueue);
    test(computeQueue);
    test(computeStdQueue);
}

void test(ComputeFunctionQueue &functionQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"function queue"};
        while (!functionQueue.empty()) { num = functionQueue.call_and_pop(num); }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::queue<Function<ComputeFunctionSig>> &computeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of functions"};
        while (!computeQueue.empty()) {
            num = computeQueue.front()(num);
            computeQueue.pop();
        }
    }
    fmt::print("result : {}\n\n", num);
}

void test(std::queue<std::function<ComputeFunctionSig>> &computeStdQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of std functions"};
        while (!computeStdQueue.empty()) {
            num = computeStdQueue.front()(num);
            computeStdQueue.pop();
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
