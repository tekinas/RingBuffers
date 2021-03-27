#include "FunctionQueue_SCSP.h"
#include "FunctionQueue_MCSP.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"

#include <thread>

using namespace util;

using ComputeFunctionSig = void();
using LockFreeQueue = FunctionQueue_SCSP<ComputeFunctionSig, true, false, false>;
//using LockFreeQueue = FunctionQueue_MCSP<ComputeFunctionSig, true, false>;

struct ComputeCxt {
private:
    size_t num{};
    size_t func{};
    size_t const num_functions;
    CallbackGenerator callbackGenerator;
    Timer timer;

public:
    explicit ComputeCxt(size_t seed, size_t num_functions, std::string_view timer_str) : num_functions{num_functions},
                                                                                         callbackGenerator{seed},
                                                                                         timer{timer_str} {}

    static void addComputeTask(std::unique_ptr<ComputeCxt> computeCxt, LockFreeQueue *functionQueue) noexcept {
        auto const cxtPtr = computeCxt.get();
        cxtPtr->callbackGenerator.addCallback(
                [computeCxt{std::move(computeCxt)}, functionQueue]<typename T>(T &&t) mutable {
                    auto compute = [computeCxt{std::move(computeCxt)}, t{std::forward<T>(t)}, functionQueue]() mutable {
                        computeCxt->num = t(computeCxt->num);
//                        std::cout << computeCxt->num << '\n';

                        if (++computeCxt->func != computeCxt->num_functions)
                            ComputeCxt::addComputeTask(std::move(computeCxt), functionQueue);
                        else println("result :", computeCxt->num);
                    };

                    while (!functionQueue->push_back(std::move(compute))) {
                        std::this_thread::yield();
                    }
                });
    }
};

int main(int argc, char **argv) {
    if (argc == 1) {
        println("usage : ./fq_test_nr_nw <buffer_size> <seed> <functions> <threads> <compute_chains>");
    }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 10; }() * 1024 * 1024;
    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 10);
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    println("total num_threads :", num_threads);

    size_t const compute_chains = [&] {
        return (argc >= 6) ? atol(argv[5]) : std::thread::hardware_concurrency();
    }();
    println("total compute chains :", compute_chains);

    LockFreeQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    for (auto t = compute_chains; t--;)
        ComputeCxt::addComputeTask(
                std::make_unique<ComputeCxt>(seed, functions, "compute chain " + std::to_string(t + 1)),
                &rawComputeQueue);

    std::vector<std::jthread> threads;
    for (auto t = num_threads; t--;)
        threads.emplace_back([&] {
            while (rawComputeQueue) rawComputeQueue.callAndPop();
        });
}