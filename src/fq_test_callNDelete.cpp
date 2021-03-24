#include "FunctionQueue.h"
#include "FunctionQueue_SCSP.h"
#include "FunctionQueue_MCSP.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"

#include <deque>
#include <folly/Function.h>

using namespace util;

using ComputeFunctionSig = size_t(size_t);
//using LockFreeQueue = FunctionQueue<true, true, ComputeFunctionSig>;
//using LockFreeQueue = FunctionQueue_SCSP<ComputeFunctionSig, false, false, false>;
using LockFreeQueue = FunctionQueue_MCSP<ComputeFunctionSig, false, false>;


using folly::Function;

using ComputeFunctionSig = size_t(size_t);

int main(int argc, char **argv) {
    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    println("using buffer of size :", rawQueueMemSize);

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize);
    LockFreeQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    std::pmr::pool_options poolOptions{};
    std::pmr::unsynchronized_pool_resource p1{poolOptions}, p2{poolOptions};
    std::pmr::deque<Function<ComputeFunctionSig>> vectorComputeQueue{&p1};
    std::pmr::deque<std::function<ComputeFunctionSig>> vectorStdComputeQueue{&p2};

    CallbackGenerator callbackGenerator{seed};

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


    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of functions fill time"};
        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) {
                        vectorComputeQueue.emplace_back(std::forward<T>(t));
                    });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"deque of std functions fill time"};
        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) {
                        vectorStdComputeQueue.emplace_back(std::forward<T>(t));
                    });
        }
    }

    println();
    println("total compute functions : ", compute_functors);
//    println("raw queue storage :", rawComputeQueue.storage_used(), " bytes");
    println("function vector storage :",
            vectorComputeQueue.size() * sizeof(decltype(vectorComputeQueue)::value_type), " bytes");
    println("std function vector storage :",
            vectorStdComputeQueue.size() * sizeof(decltype(vectorStdComputeQueue)::value_type), " bytes");

    println();

    size_t num = 0;
    {
        Timer timer{"function queue"};
        while (rawComputeQueue) {
            num = rawComputeQueue.callAndPop(num);
        }
    }
    println("result :", num, '\n');

    extern void test(std::pmr::deque<Function<ComputeFunctionSig>> &);
    extern void test(std::pmr::deque<std::function<ComputeFunctionSig>> &);

    test(vectorComputeQueue);
    test(vectorStdComputeQueue);
}

void test(std::pmr::deque<Function<ComputeFunctionSig>> &vectorComputeQueue) {
    size_t num = 0;
    {
        Timer timer{"deque of functions"};
        while (!vectorComputeQueue.empty()) {
            num = vectorComputeQueue.front()(num);
            vectorComputeQueue.pop_front();
        }
    }
    println("result :", num, '\n');
}


void test(std::pmr::deque<std::function<ComputeFunctionSig>> &vectorStdComputeQueue) {
    size_t num = 0;
    {
        Timer timer{"deque of std functions"};
        while (!vectorStdComputeQueue.empty()) {
            num = vectorStdComputeQueue.front()(num);
            vectorStdComputeQueue.pop_front();
        }
    }
    println("result :", num, '\n');
}


