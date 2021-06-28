#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "../ObjectQueue_MCSP.h"
#include "util.h"
#include <boost/container_hash/hash_fwd.hpp>
#include <cstring>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <boost/container_hash/hash.hpp>

#include <mutex>
#include <thread>
#include <vector>

using util::Random;
using util::Timer;

struct Obj {
    uint32_t a;
    float b;
    uint64_t c;

    Obj(Obj const &) = default;

    explicit Obj(Random<> &rng) noexcept
        : a{rng.getRand<uint32_t>(1, 99999 + 0b10101010101)}, b{rng.getRand<float>(-1.13242424f, 788978.0f)},
          c{rng.getRand<uint64_t>(0, 835454325463)} {}

    /*uint64_t operator()(Random<> &rng) const noexcept {
        size_t hash{};
        boost::hash_combine(hash, a);
        boost::hash_combine(hash, b);
        boost::hash_combine(hash, c);
        return hash;
    }*/

    uint64_t operator()(Random<> &rng) const noexcept {
        rng.setSeed(a);
        return rng.getRand<uint32_t>(0, a) * std::bit_cast<uint32_t>(rng.getRand(-b, b)) * rng.getRand<uint64_t>(0, c);
    }
};

bool OQ_IsObjectFree(Obj *ptr) noexcept {
    return reinterpret_cast<std::atomic<uint32_t> &>(ptr->a).load(std::memory_order_acquire) == 0;
}

void OQ_FreeObject(Obj *ptr) noexcept {
    reinterpret_cast<std::atomic<uint32_t> &>(ptr->a).store(0, std::memory_order_release);
}

using ObjectQueue = ObjectQueue_MCSP<Obj, false>;
using FunctionQueue = FunctionQueue_MCSP<uint64_t(Random<> &), false, false>;
//using FunctionQueue = FunctionQueue_SCSP<uint64_t(Random<> &), true, false, false>;

void test(ObjectQueue &objectQueue, uint16_t threads, uint32_t objects, std::size_t seed) noexcept {
    std::vector<uint64_t> final_result;
    std::mutex final_result_mutex;
    std::vector<std::jthread> reader_threads;
    std::atomic<bool> is_done{false};

    for (uint16_t i = 0; i != threads; ++i)
        reader_threads.emplace_back(
                [&objectQueue, &is_done, &final_result, &final_result_mutex, i, object_per_thread = objects / threads] {
                    constexpr std::size_t seed = 0;
                    Random<> rng{seed};

                    std::vector<uint64_t> local_result;
                    local_result.reserve(object_per_thread);

                    {
                        Timer timer{"read time "};

                        while (true) {
                            if (auto ptr = objectQueue.consume()) {
                                local_result.push_back((*ptr)(rng));
                            } else if (is_done.load(std::memory_order_relaxed))
                                break;
                            else
                                std::this_thread::yield();

                            /*auto const consumed =
                            objectQueue.consume_all([&](Obj &obj) mutable { local_result.push_back(obj(rng)); });
                    if (!consumed) {
                        if (is_done.load(std::memory_order_relaxed)) break;
                        else
                            std::this_thread::yield();
                    }*/
                        }
                    }

                    fmt::print("thread {} finished.\n", i);

                    std::scoped_lock lock{final_result_mutex};
                    final_result.insert(final_result.end(), local_result.begin(), local_result.end());
                });

    std::jthread writer{[&objectQueue, objects, &is_done, seed] {
        Random<> rng{seed};

        auto obj = objects;
        while (obj) {
            while (!objectQueue.emplace_back(rng)) std::this_thread::yield();
            --obj;
        }

        fmt::print("writer thread finished, objects processed : {}\n", objects);
        is_done.store(true, std::memory_order_release);
    }};

    for (auto &thread : reader_threads) thread.join();

    fmt::print("numbers in result vector : {}\n", final_result.size());
    std::sort(final_result.begin(), final_result.end());
    fmt::print("hash result : {}\n", boost::hash_range(final_result.begin(), final_result.end()));
}

void test(FunctionQueue &functionQueue, uint16_t threads, uint32_t objects, std::size_t seed) noexcept {
    std::vector<uint64_t> final_result;
    std::mutex final_result_mutex;
    std::vector<std::jthread> reader_threads;
    std::atomic<bool> is_done{false};

    for (uint16_t i = 0; i != threads; ++i)
        reader_threads.emplace_back([&functionQueue, &is_done, &final_result, &final_result_mutex, i,
                                     object_per_thread = objects / threads] {
            constexpr std::size_t seed = 0;
            Random<> rng{seed};

            std::vector<uint64_t> local_result;
            local_result.reserve(object_per_thread);

            {
                Timer timer{"read time "};

                while (true) {
                    if (auto handle = functionQueue.get_function_handle()) {
                        local_result.push_back(handle.call_and_pop(rng));
                    } else if (is_done.load(std::memory_order_relaxed))
                        break;
                    else
                        std::this_thread::yield();

                    /*if (functionQueue.reserve()) {
                        local_result.push_back(functionQueue.call_and_pop(rng));
                    } else if (is_done.load(std::memory_order_relaxed))
                        break;
                    else
                        std::this_thread::yield();*/
                }
            }

            fmt::print("thread {} finished.\n", i);

            std::scoped_lock lock{final_result_mutex};
            final_result.insert(final_result.end(), local_result.begin(), local_result.end());
        });

    std::jthread writer{[&functionQueue, objects, &is_done, seed] {
        Random<> rng{seed};

        auto obj = objects;
        while (obj) {
            //while (!functionQueue.emplace_back<Obj>(rng)) std::this_thread::yield();

            auto func = [obj = Obj{rng}](Random<> &rng) { return obj(rng); };
            while (!functionQueue.push_back(func)) std::this_thread::yield();
            --obj;
        }

        fmt::print("writer thread finished, objects processed : {}\n", objects);
        is_done.store(true, std::memory_order_release);
    }};

    for (auto &thread : reader_threads) thread.join();

    fmt::print("numbers in result vector : {}\n", final_result.size());
    std::sort(final_result.begin(), final_result.end());
    fmt::print("hash result : {}\n", boost::hash_range(final_result.begin(), final_result.end()));
}

int main(int argc, char **argv) {
    constexpr uint32_t object_count = 10'0000;

    size_t const objects = [&] { return (argc >= 2) ? atol(argv[1]) : 10'000'000; }();
    fmt::print("objects to process : {}\n", objects);

    size_t const num_threads = [&] { return (argc >= 3) ? atol(argv[2]) : std::thread::hardware_concurrency(); }();
    fmt::print("reader threads : {}\n", num_threads);

    size_t const seed = [&] { return (argc >= 4) ? atol(argv[3]) : 100; }();
    fmt::print("seed : {}\n", seed);

    auto buffer = std::make_unique<std::aligned_storage_t<sizeof(Obj), alignof(Obj)>[]>(object_count);
    ObjectQueue objectQueue{reinterpret_cast<Obj *>(buffer.get()), object_count};

    size_t functionQueueBufferSize = static_cast<size_t>(sizeof(Obj) * object_count * 5.0 / 3.0);
    auto functionQueueBuffer = std::make_unique<std::byte[]>(functionQueueBufferSize);
    FunctionQueue functionQueue{functionQueueBuffer.get(), functionQueueBufferSize};

    fmt::print("Function Queue test ....\n");
    test(functionQueue, num_threads, objects, seed);

    fmt::print("\n\nObject Queue test ....\n");
    test(objectQueue, num_threads, objects, seed);
}
