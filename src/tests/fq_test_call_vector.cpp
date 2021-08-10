#include "../FunctionQueue.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <bit>
#include <chrono>
#include <folly/Function.h>
#include <random>
#include <type_traits>

#include <fmt/format.h>

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue<ComputeFunctionSig, false>;

using folly::Function;
using util::Timer;

size_t default_alloc{};
size_t *bytes_allocated = &default_alloc;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_call_only <buffer_size> <seed>\n"); }

    size_t const functionQueueBufferSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", functionQueueBufferSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);

    std::vector<Function<ComputeFunctionSig>> follyFunctionVector{};
    std::vector<std::function<ComputeFunctionSig>> stdFuncionVector{};

    CallbackGenerator callbackGenerator{seed};

    auto const functionQueueBuffer =
            std::make_unique<std::aligned_storage_t<ComputeFunctionQueue::BUFFER_ALIGNMENT, sizeof(std::byte)>[]>(
                    functionQueueBufferSize);
    ComputeFunctionQueue functionQueue{std::bit_cast<std::byte *>(functionQueueBuffer.get()), functionQueueBufferSize};

    size_t const compute_functors = [&] {
        Timer timer{"function queue write time"};

        uint32_t functions{0};
        bool addFunction = true;
        while (addFunction) {
            ++functions;
            callbackGenerator.addCallback(util::overload(
                    [&]<typename T>(T &&t) { addFunction = functionQueue.push_back(std::forward<T>(t)); },
                    [&]<auto fp> { addFunction = functionQueue.push_back<fp>(); }));
        }
        --functions;

        return functions;
    }();

    follyFunctionVector.reserve(compute_functors);
    stdFuncionVector.reserve(compute_functors);

    size_t follyFunctionAllocs{0}, stdFunctionAllocs{0};
    callbackGenerator.setSeed(seed);
    {
        Timer timer{"std::vector<folly::Functions> write time"};
        bytes_allocated = &follyFunctionAllocs;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { follyFunctionVector.emplace_back(std::forward<T>(t)); });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"std::vector<std::functions> write time"};
        bytes_allocated = &stdFunctionAllocs;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback(
                    [&]<typename T>(T &&t) { stdFuncionVector.emplace_back(std::forward<T>(t)); });
        }
    }

    fmt::print("\ncompute functions : {}\n\n", compute_functors);

    constexpr double ONE_MiB = 1024.0 * 1024.0;
    fmt::print("std::vector<folly::Function> memory footprint : {} MiB\n",
               (follyFunctionVector.size() * sizeof(folly::Function<ComputeFunctionSig>) + follyFunctionAllocs) /
                       ONE_MiB);
    fmt::print("std::vector<std::function> memory footprint : {} MiB\n",
               (stdFuncionVector.size() * sizeof(std::function<ComputeFunctionSig>) + stdFunctionAllocs) / ONE_MiB);
    fmt::print("function queue memory : {} MiB\n\n", functionQueue.buffer_size() / ONE_MiB);

    void test(ComputeFunctionQueue &) noexcept;
    void test(std::vector<Function<ComputeFunctionSig>> &) noexcept;
    void test(std::vector<std::function<ComputeFunctionSig>> &) noexcept;

    test(follyFunctionVector);
    test(stdFuncionVector);
    test(functionQueue);
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
        Timer timer{"std::vector<folly::Function>"};

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
        Timer timer{"std::vector<std::function>"};

        for (auto &function : vectorStdComputeQueue) {
            num = function(num);
            //function = {};
        }
    }
    fmt::print("result : {}\n\n", num);
}

void *operator new(size_t bytes) {
    *bytes_allocated += bytes;
    return malloc(bytes);
}

void operator delete(void *ptr, size_t) { free(ptr); }
void operator delete(void *ptr) { free(ptr); }
