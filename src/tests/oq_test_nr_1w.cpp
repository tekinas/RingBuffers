#include "util.h"

#include <RingBuffers/BufferQueue_MCSP.h>
#include <RingBuffers/FunctionQueue_MCSP.h>
#include <RingBuffers/ObjectQueue_MCSP.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "boost/lockfree/policies.hpp"
#include <boost/container_hash/hash.hpp>
#include <boost/lockfree/queue.hpp>


using util::Random;
using util::StartFlag;
using util::Timer;

class Obj {
public:
    using RNG = Random<boost::random::mt19937_64>;

    Obj() noexcept = default;

    explicit Obj(RNG &rng) noexcept
        : a{rng.getRand<uint64_t>(std::numeric_limits<uint64_t>::min(), std::numeric_limits<uint64_t>::max())},
          b{rng.getRand<float>(std::numeric_limits<float>::min(), std::numeric_limits<float>::max())},
          c{rng.getRand<uint32_t>(std::numeric_limits<uint32_t>::min(), std::numeric_limits<uint32_t>::max())} {}

    uint64_t operator()(Obj::RNG &rng) const noexcept {
        auto seed = a;
        rng.setSeed(seed);
        auto const aa = rng.getRand<uint64_t>(0, a);
        auto const bb = std::bit_cast<uint32_t>(rng.getRand(-b, b));
        auto const cc = rng.getRand<uint32_t>(0, c);

        boost::hash_combine(seed, aa);
        boost::hash_combine(seed, bb);
        boost::hash_combine(seed, cc);
        return seed;
    }

private:
    uint64_t a;
    float b;
    uint32_t c;
};

using BoostQueue = boost::lockfree::queue<Obj, boost::lockfree::fixed_sized<true>>;
using ObjectQueue = ObjectQueue_MCSP<Obj>;
using FunctionQueue = FunctionQueue_MCSP<uint64_t(Obj::RNG &), false, alignof(Obj) + sizeof(Obj)>;
using BufferQueue = BufferQueue_MCSP<false, alignof(Obj)>;

template<typename ObjectQueueType>
requires std::same_as<ObjectQueueType, BoostQueue> || std::same_as<ObjectQueueType, ObjectQueue>
void test(ObjectQueueType &objectQueue, uint16_t threads, uint32_t objects, std::size_t seed) noexcept {
    std::vector<uint64_t> final_result;
    std::mutex final_result_mutex;

    std::atomic<bool> is_done{false};
    StartFlag start_flag;

    std::vector<std::jthread> reader_threads;
    for (uint16_t i = 0; i != threads; ++i)
        reader_threads.emplace_back(
                [&, i, object_per_thread = objects / threads, rng = Obj::RNG{seed}]() mutable noexcept {
                    start_flag.wait();

                    std::vector<uint64_t> local_result;
                    local_result.reserve(object_per_thread);

                    {
                        Timer timer{"read time "};

                        if constexpr (std::same_as<ObjectQueueType, BoostQueue>)
                            while (true) {
                                if (Obj obj; objectQueue.pop(obj)) local_result.push_back(obj(rng));
                                else if (is_done.load(std::memory_order_relaxed))
                                    break;
                                else
                                    std::this_thread::yield();
                            }
                        if constexpr (std::same_as<ObjectQueueType, ObjectQueue>)
                            while (true) {
                                if (auto ptr = objectQueue.consume()) {
                                    local_result.push_back((*ptr)(rng));
                                } else if (is_done.load(std::memory_order_relaxed))
                                    break;
                                else
                                    std::this_thread::yield();
                            }
                    }

                    fmt::print("thread {} finished.\n", i);

                    std::scoped_lock lock{final_result_mutex};
                    final_result.insert(final_result.end(), local_result.begin(), local_result.end());
                });

    std::jthread writer{[&, objects, rng = Obj::RNG{seed}]() mutable noexcept {
        start_flag.wait();

        auto o = objects;
        while (o) {
            Obj obj{rng};
            while (!objectQueue.push(obj)) {
                std::this_thread::yield();
                if constexpr (std::same_as<ObjectQueueType, ObjectQueue>) objectQueue.clean_memory();
            }
            --o;
        }

        fmt::print("writer thread finished, objects processed : {}\n", objects);
        is_done.store(true, std::memory_order_release);
    }};

    start_flag.start();
    for (auto &thread : reader_threads) thread.join();

    fmt::print("numbers in result vector : {}\n", final_result.size());
    std::sort(final_result.begin(), final_result.end());
    fmt::print("hash result : {}\n", boost::hash_range(final_result.begin(), final_result.end()));
}

void test(FunctionQueue &functionQueue, uint16_t threads, uint32_t objects, std::size_t seed) noexcept {
    std::vector<uint64_t> final_result;
    std::mutex final_result_mutex;

    std::atomic<bool> is_done{false};
    StartFlag start_flag;

    std::vector<std::jthread> reader_threads;
    for (uint16_t i = 0; i != threads; ++i)
        reader_threads.emplace_back(
                [&, i, object_per_thread = objects / threads, rng = Obj::RNG{seed}]() mutable noexcept {
                    start_flag.wait();

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
                        }
                    }

                    fmt::print("thread {} finished.\n", i);

                    std::scoped_lock lock{final_result_mutex};
                    final_result.insert(final_result.end(), local_result.begin(), local_result.end());
                });

    std::jthread writer{[&, objects, rng = Obj::RNG{seed}]() mutable noexcept {
        start_flag.wait();

        auto o = objects;
        while (o) {
            Obj obj{rng};
            while (!functionQueue.push(obj)) {
                std::this_thread::yield();
                functionQueue.clean_memory();
            }
            --o;
        }

        fmt::print("writer thread finished, objects processed : {}\n", objects);
        is_done.store(true, std::memory_order_release);
    }};

    start_flag.start();
    for (auto &thread : reader_threads) thread.join();

    fmt::print("numbers in result vector : {}\n", final_result.size());
    std::sort(final_result.begin(), final_result.end());
    fmt::print("hash result : {}\n", boost::hash_range(final_result.begin(), final_result.end()));
}

void test(BufferQueue &bufferQueue, uint16_t threads, uint32_t objects, std::size_t seed) noexcept {
    std::vector<uint64_t> final_result;
    std::mutex final_result_mutex;

    std::atomic<bool> is_done{false};
    StartFlag start_flag;

    std::vector<std::jthread> reader_threads;
    for (uint16_t i = 0; i != threads; ++i)
        reader_threads.emplace_back(
                [&, i, object_per_thread = objects / threads, rng = Obj::RNG{seed}]() mutable noexcept {
                    start_flag.wait();

                    std::vector<uint64_t> local_result;
                    local_result.reserve(object_per_thread);

                    {
                        Timer timer{"read time "};

                        while (true) {
                            if (auto data_buffer = bufferQueue.consume()) {
                                auto &object = *std::bit_cast<Obj *>(data_buffer.get().data());
                                local_result.push_back(object(rng));
                            } else if (is_done.load(std::memory_order_relaxed))
                                break;
                            else
                                std::this_thread::yield();
                        }
                    }

                    fmt::print("thread {} finished.\n", i);

                    std::scoped_lock lock{final_result_mutex};
                    final_result.insert(final_result.end(), local_result.begin(), local_result.end());
                });

    std::jthread writer{[&, objects, rng = Obj::RNG{seed}]() mutable noexcept {
        start_flag.wait();

        auto o = objects;
        while (o) {
            auto const obj_creator = [obj = Obj{rng}](std::span<std::byte> buffer) noexcept {
                std::construct_at(std::bit_cast<Obj *>(buffer.data()), obj);
                return sizeof(Obj);
            };

            while (!bufferQueue.allocate_and_release(sizeof(Obj), obj_creator)) std::this_thread::yield();
            --o;
        }

        fmt::print("writer thread finished, objects processed : {}\n", objects);
        is_done.store(true, std::memory_order_release);
    }};

    start_flag.start();
    for (auto &thread : reader_threads) thread.join();

    fmt::print("numbers in result vector : {}\n", final_result.size());
    std::sort(final_result.begin(), final_result.end());
    fmt::print("hash result : {}\n", boost::hash_range(final_result.begin(), final_result.end()));
}

int main(int argc, char **argv) {
    constexpr uint32_t object_count = 65'534;

    size_t const objects = [&] { return (argc >= 2) ? atol(argv[1]) : 10'000'000; }();
    fmt::print("objects to process : {}\n", objects);

    size_t const num_threads = [&] { return (argc >= 3) ? atol(argv[2]) : std::thread::hardware_concurrency(); }();
    fmt::print("reader threads : {}\n", num_threads);

    size_t const seed = [&] { return (argc >= 4) ? atol(argv[3]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);

    auto buffer = std::make_unique<
            std::aligned_storage_t<sizeof(Obj), std::max(FunctionQueue::BUFFER_ALIGNMENT, alignof(Obj))>[]>(
            object_count);

    {
        fmt::print("\nBoost Queue test ....\n");
        BoostQueue boostqueue{object_count};
        test(boostqueue, num_threads, objects, seed);
    }

    {
        fmt::print("\nObject Queue test ....\n");
        auto const clean_array = std::make_unique<std::atomic<bool>[]>(object_count);
        ObjectQueue objectQueue{reinterpret_cast<Obj *>(buffer.get()), clean_array.get(), object_count};
        test(objectQueue, num_threads, objects, seed);
    }

    {
        fmt::print("\nFunction Queue test ....\n");
        constexpr size_t buffer_size = object_count * sizeof(Obj);
        auto cleanOffsetArray = std::make_unique<std::atomic<uint16_t>[]>(FunctionQueue::clean_array_size(buffer_size));
        FunctionQueue functionQueue{std::bit_cast<std::byte *>(buffer.get()), buffer_size, cleanOffsetArray.get()};
        test(functionQueue, num_threads, objects, seed);
    }

    {
        fmt::print("\nBuffer Queue test ....\n");
        BufferQueue bufferQueue{std::bit_cast<std::byte *>(buffer.get()), object_count * sizeof(Obj)};
        test(bufferQueue, num_threads, objects, seed);
    }
}
