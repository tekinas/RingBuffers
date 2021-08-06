#include "../FunctionQueue.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"

#include <boost/circular_buffer.hpp>
#include <folly/Function.h>
#include <functional>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

using ComputeFunctionSig = size_t(size_t);
using ComputeFunctionQueue = FunctionQueue<ComputeFunctionSig, false>;

using util::Timer;

size_t *bytes_allocated = nullptr;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_call_and_pop <buffer_size> <seed>\n"); }

    size_t const functionQueueBufferSize = [&] { return (argc >= 2) ? atof(argv[1]) : 500.0; }() * 1024 * 1024;
    fmt::print("buffer size : {}\n", functionQueueBufferSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);


    auto const functionQueueBuffer =
            std::make_unique<std::aligned_storage_t<ComputeFunctionQueue::BUFFER_ALIGNMENT, sizeof(std::byte)>[]>(
                    functionQueueBufferSize);
    ComputeFunctionQueue functionQueue{std::bit_cast<std::byte *>(functionQueueBuffer.get()), functionQueueBufferSize};


    CallbackGenerator callbackGenerator{seed};
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


    boost::circular_buffer<folly::Function<ComputeFunctionSig>> computeQueue{compute_functors};
    boost::circular_buffer<std::function<ComputeFunctionSig>> computeStdQueue{compute_functors};

    size_t follyFunctionAllocs{0}, stdFunctionAllocs{0};
    callbackGenerator.setSeed(seed);
    {
        Timer timer{"boost::circular_buffer of folly::Functions fill time"};
        bytes_allocated = &follyFunctionAllocs;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeQueue.push_back(std::forward<T>(t)); });
        }
    }

    callbackGenerator.setSeed(seed);
    {
        Timer timer{"boost::circular_buffer of std::functions fill time"};
        bytes_allocated = &stdFunctionAllocs;

        for (auto count = compute_functors; count--;) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) { computeStdQueue.push_back(std::forward<T>(t)); });
        }
    }

    fmt::print("\ncompute functions : {}\n\n", compute_functors);

    constexpr double ONE_MiB = 1024.0 * 1024.0;
    fmt::print("boost::circular_buffer<folly::Function> memory : {} MiB\n",
               (follyFunctionAllocs + computeQueue.size() * sizeof(folly::Function<ComputeFunctionSig>)) / ONE_MiB);
    fmt::print("boost::circular_buffer<std::function> memory : {} MiB\n",
               (stdFunctionAllocs + computeStdQueue.size() * sizeof(std::function<ComputeFunctionSig>)) / ONE_MiB);
    fmt::print("function queue memory : {} MiB\n\n", functionQueue.buffer_size() / ONE_MiB);

    void test(boost::circular_buffer<folly::Function<ComputeFunctionSig>> &) noexcept;
    void test(boost::circular_buffer<std::function<ComputeFunctionSig>> &) noexcept;
    void test(ComputeFunctionQueue &) noexcept;

    test(computeQueue);
    test(computeStdQueue);
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

void test(boost::circular_buffer<folly::Function<ComputeFunctionSig>> &computeQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"boost::circular_buffer of folly::Functions"};
        while (!computeQueue.empty()) {
            num = computeQueue.front()(num);
            computeQueue.pop_front();
        }
    }
    fmt::print("result : {}\n\n", num);
}

void test(boost::circular_buffer<std::function<ComputeFunctionSig>> &computeStdQueue) noexcept {
    size_t num = 0;
    {
        Timer timer{"boost::circular_buffer of std::functions"};
        while (!computeStdQueue.empty()) {
            num = computeStdQueue.front()(num);
            computeStdQueue.pop_front();
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
