#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <fmt/format.h>

#include <thread>

using util::Timer;

using ComputeFunctionSig = void();
using ComputeFunctionQueue = FunctionQueue_SCSP<ComputeFunctionSig, true, false, false>;
//using ComputeFunctionQueue = FunctionQueue_MCSP<ComputeFunctionSig, true, false>;

struct ComputeCxt {
private:
    size_t num{};
    size_t func{};
    size_t const num_functions;
    CallbackGenerator callbackGenerator;
    Timer timer;

public:
    explicit ComputeCxt(size_t seed, size_t num_functions, std::string_view timer_str)
        : num_functions{num_functions}, callbackGenerator{seed}, timer{timer_str} {}

    static void addComputeTask(std::unique_ptr<ComputeCxt> computeCxt, ComputeFunctionQueue *functionQueue) noexcept {
        auto const cxtPtr = computeCxt.get();
        cxtPtr->callbackGenerator.addCallback(
                [computeCxt{std::move(computeCxt)}, functionQueue]<typename T>(T &&t) mutable {
                    auto compute = [computeCxt{std::move(computeCxt)}, t{std::forward<T>(t)},
                                    functionQueue]() mutable noexcept {
                        computeCxt->num = t(computeCxt->num);

                        if (++computeCxt->func != computeCxt->num_functions)
                            ComputeCxt::addComputeTask(std::move(computeCxt), functionQueue);
                        else
                            fmt::print("result : {}\n", computeCxt->num);
                    };

                    while (!functionQueue->push_back(std::move(compute))) { std::this_thread::yield(); }
                });
    }
};

int main(int argc, char **argv) {
    if (argc == 1) {
        fmt::print("usage : ./fq_test_nr_nw <buffer_size> <seed> <functions> "
                   "<threads> <compute_chains>\n");
    }

    size_t const rawQueueMemSize = [&] { return (argc >= 2) ? atof(argv[1]) : 10; }() * 1024 * 1024;
    auto const rawQueueMem = std::make_unique<std::byte[]>(rawQueueMemSize + 10);
    fmt::print("buffer size : {}\n", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    fmt::print("functions : {}\n", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    fmt::print("threads : {}\n", num_threads);

    size_t const compute_chains = [&] { return (argc >= 6) ? atol(argv[5]) : std::thread::hardware_concurrency(); }();
    fmt::print("compute chains : {}\n", compute_chains);

    ComputeFunctionQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    for (auto t = compute_chains; t--;)
        ComputeCxt::addComputeTask(
                std::make_unique<ComputeCxt>(seed, functions, "compute chain " + std::to_string(t + 1)),
                &rawComputeQueue);

    std::vector<std::jthread> threads;
    for (auto t = num_threads; t--;)
        threads.emplace_back([&] {
            while (rawComputeQueue.reserve()) rawComputeQueue.call_and_pop();
        });
}
