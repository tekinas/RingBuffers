#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "ComputeCallbackGenerator.h"
#include "util.h"
#include <random>

#include <fmt/format.h>

#include <thread>

using ComputeFunctionSig = size_t(size_t);
using FunctionQueueSCSP = FunctionQueue_SCSP<ComputeFunctionSig, false, false, false>;
using FunctionQueueMCSP = FunctionQueue_MCSP<ComputeFunctionSig, false, false>;
using FunctionQueueType = FunctionQueueMCSP;

template<typename Test, template<typename, auto...> class Ref>
struct is_specialization : std::false_type {};

template<template<typename, auto...> class Ref, typename T, auto... args>
struct is_specialization<Ref<T, args...>, Ref> : std::true_type {};

template<typename FunctionQueueType>
class FunctionQueueData {
private:
    std::unique_ptr<std::byte[]> functionQueueBuffer;
    [[no_unique_address]] std::conditional_t<std::same_as<FunctionQueueType, FunctionQueueMCSP>,
                                             std::unique_ptr<std::atomic<uint16_t>[]>, std::monostate>
            cleanOffsetArray;

public:
    FunctionQueueType functionQueue;

    template<typename = void>
    requires is_specialization<FunctionQueueType, FunctionQueue_MCSP>::value FunctionQueueData(size_t buffer_size)
        : functionQueueBuffer{std::make_unique<std::byte[]>(buffer_size)},
          cleanOffsetArray{std::make_unique<std::atomic<uint16_t>[]>(FunctionQueueType::clean_array_size(buffer_size))},
          functionQueue{functionQueueBuffer.get(), buffer_size, cleanOffsetArray.get()} {}

    template<typename = void>
    requires is_specialization<FunctionQueueType, FunctionQueue_SCSP>::value FunctionQueueData(size_t buffer_size)
        : functionQueueBuffer{std::make_unique<std::byte[]>(buffer_size)}, functionQueue{functionQueueBuffer.get(),
                                                                                         buffer_size} {}
};

using util::Timer;

template<typename FunctionQueue>
void test(FunctionQueue &functionQueue, CallbackGenerator &callbackGenerator, size_t functions) noexcept {
    util::StartFlag start_flag;

    std::jthread reader{[&] {
        start_flag.wait();
        size_t num{0}, res{0};
        {
            Timer timer{"reader"};
            while (res != std::numeric_limits<size_t>::max()) {
                num = res;
                if constexpr (std::is_same_v<FunctionQueueSCSP, FunctionQueue>) {
                    if (functionQueue.reserve()) {
                        res = functionQueue.call_and_pop(res);
                    } else {
                        std::this_thread::yield();
                    }
                } else {
                    if (auto handle = functionQueue.get_function_handle()) {
                        res = handle.call_and_pop(res);
                    } else {
                        std::this_thread::yield();
                    }
                }
            }
        }
        fmt::print("result : {}\n", num);
        functionQueue.clear();
    }};

    std::jthread writer{[&] {
        start_flag.wait();
        auto func = functions;
        while (func) {
            callbackGenerator.addCallback([&]<typename T>(T &&t) {
                while (!functionQueue.push_back(std::forward<T>(t))) { std::this_thread::yield(); }
                --func;
            });
        }

        while (!functionQueue.push_back([](auto) noexcept { return std::numeric_limits<size_t>::max(); }))
            ;
    }};

    start_flag.start();
}

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./fq_test_1r_1w <buffer_size> <seed> <functions>\n"); }

    size_t const buffer_size = [&] { return (argc >= 2) ? atof(argv[1]) : 1000.0 / 1024.0 / 1024.0; }() * 1024 * 1024;

    fmt::print("buffer size : {}\n", buffer_size);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 20'000'000; }();
    fmt::print("functions : {}\n", functions);

    FunctionQueueData<FunctionQueueType> functionQueueData{buffer_size};

    CallbackGenerator callbackGenerator{seed};

    test(functionQueueData.functionQueue, callbackGenerator, functions);
}
