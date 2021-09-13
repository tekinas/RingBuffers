#include "ComputeCallbackGenerator.h"
#include "util.h"
#include <RingBuffers/FunctionQueue_MCSP.h>
#include <RingBuffers/FunctionQueue_SCSP.h>
#include <limits>
#include <random>

#include <fmt/format.h>

#include <thread>

using namespace rb;
using ComputeFunctionSig = size_t(size_t);
using FunctionQueueSCSP = FunctionQueue_SCSP<ComputeFunctionSig, false>;
using FunctionQueueMCSP = FunctionQueue_MCSP<ComputeFunctionSig, 1, false>;
using FunctionQueueType = FunctionQueueMCSP;

template<typename FQType>
auto makeFunctionQueue(size_t buffer_size, uint16_t threads = 1) noexcept {
    if constexpr (std::same_as<FunctionQueueMCSP, FQType>) return FQType{static_cast<uint32_t>(buffer_size), threads};
    else
        return FQType{buffer_size};
}

using util::Timer;

template<typename FunctionQueue>
void test(FunctionQueue &functionQueue, CallbackGenerator &callbackGenerator, size_t functions) noexcept {
    util::StartFlag start_flag;

    std::jthread reader{[&] {
        start_flag.wait();
        size_t num{0};
        {
            Timer timer{"reader"};
            if constexpr (std::is_same_v<FunctionQueueSCSP, FunctionQueue>) {
                while (true) {
                    if (!functionQueue.empty())
                        if (auto const res = functionQueue.call_and_pop(num); res != std::numeric_limits<size_t>::max())
                            num = res;
                        else
                            break;
                    else
                        std::this_thread::yield();
                }
            } else {
                auto reader = functionQueue.getReader(0);
                while (true) {
                    if (auto function_handle = reader.get_function_handle())
                        if (auto const res = function_handle.call_and_pop(num);
                            res != std::numeric_limits<size_t>::max())
                            num = res;
                        else
                            break;
                    else
                        std::this_thread::yield();
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
                while (!functionQueue.push(std::forward<T>(t))) {
                    std::this_thread::yield();
                    if constexpr (std::is_same_v<FunctionQueueMCSP, FunctionQueue>) functionQueue.clean_memory();
                }
                --func;
            });
        }

        while (!functionQueue.push([](auto) noexcept { return std::numeric_limits<size_t>::max(); }))
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

    auto functionQueue = makeFunctionQueue<FunctionQueueType>(buffer_size);

    CallbackGenerator callbackGenerator{seed};

    test(functionQueue, callbackGenerator, functions);
}
