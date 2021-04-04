#include "FunctionQueue.h"
#include "FunctionQueue_SCSP.h"
#include "FunctionQueue_MCSP.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"

#include <deque>
#include <folly/Function.h>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue<ComputeFunctionSig, false>;
//using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, false, false>;
//using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, false, false, false>;


using folly::Function;

size_t *bytes_allocated = nullptr;

int main(int argc, char **argv) {
    if (argc == 1) {
        println("usage : ./fq_test_call_and_pop <buffer_size> <seed>");
    }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    std::deque<Function<ComputeFunctionSig>> computeDequeue;
    std::deque<std::function<ComputeFunctionSig>> computeStdDequeue;

    CallbackGenerator callbackGenerator{seed};

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize);
    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    size_t const compute_functors =
            [&] {
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
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) {
                        computeDequeue.emplace_back(std::forward<T>(t));
                    });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of std functions fill time"};
        bytes_allocated = &computeStdDequeueStorage;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) {
                        computeStdDequeue.emplace_back(std::forward<T>(t));
                    });
        }
    }

    println();
    println("total compute functions : ", compute_functors);
    constexpr double ONE_MB = 1024.0 * 1024.0;
    println("function queue storage :", rawQueueMemSize / ONE_MB, " Mb");
    println("std::dequeue<folly::Function> storage :", computeDequeueStorage / ONE_MB, " Mb");
    println("std::dequeue<std::function> storage :", computeStdDequeueStorage / ONE_MB, " Mb");

    println();

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
        while (rawComputeQueue.reserve_function()) {
            num = rawComputeQueue.call_and_pop(num);
        }
    }
    println("result :", num, '\n');
}

void test(std::deque<Function<ComputeFunctionSig>> &computeDequeue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of functions"};
        for (auto end = computeDequeue.end(), begin = computeDequeue.begin(); begin != end; ++begin) {
            num = (*begin)(num);
        }
    }
    println("result :", num, '\n');
}


void test(std::deque<std::function<ComputeFunctionSig>> &computeStdDequeue) noexcept {
    size_t num = 0;
    {
        Timer timer{"std::deque of std functions"};
        for (auto end = computeStdDequeue.end(), begin = computeStdDequeue.begin(); begin != end; ++begin) {
            num = (*begin)(num);
        }
    }
    println("result :", num, '\n');
}

void *operator new(size_t bytes) {
    if (bytes_allocated) *bytes_allocated += bytes;
    return malloc(bytes);
}

void operator delete(void *ptr) {
    free(ptr);
}