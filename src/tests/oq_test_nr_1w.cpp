#include "../BufferQueue_MCSP.h"
#include "../FunctionQueue_MCSP.h"
#include "../ObjectQueue_MCSP.h"
#include "util.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <boost/container_hash/hash.hpp>

#include <mutex>
#include <thread>
#include <vector>

using util::Random;
using util::StartFlag;
using util::Timer;

class Obj {
public:
    using RNG = Random<boost::random::mt19937_64>;
    explicit Obj(RNG &rng) noexcept
        : a{rng.getRand<uint64_t>(std::numeric_limits<uint64_t>::min() + 1, std::numeric_limits<uint64_t>::max())},
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
    friend bool OQ_IsObjectFree(Obj *ptr) noexcept;
    friend void OQ_FreeObject(Obj *ptr) noexcept;

    uint64_t a;
    float b;
    uint32_t c;
};

bool OQ_IsObjectFree(Obj *ptr) noexcept {
    return std::bit_cast<std::atomic<uint64_t> *>(&std::launder(ptr)->a)->load(std::memory_order_acquire) == 0;
}

void OQ_FreeObject(Obj *ptr) noexcept {
    std::bit_cast<std::atomic<uint64_t> *>(&std::launder(ptr)->a)->store(0, std::memory_order_release);
}

using ObjectQueue = ObjectQueue_MCSP<Obj, false>;
using FunctionQueue = FunctionQueue_MCSP<uint64_t(Obj::RNG &), false, false>;
using BufferQueue = BufferQueue_MCSP<false, alignof(Obj)>;

void test(ObjectQueue &objectQueue, uint16_t threads, uint32_t objects, std::size_t seed) noexcept {
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

    std::jthread writer{[&, rng = Obj::RNG{seed}]() mutable noexcept {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            while (!objectQueue.emplace_back(rng)) std::this_thread::yield();
            --obj;
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

    std::jthread writer{[&, rng = Obj::RNG{seed}]() mutable noexcept {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            Obj object{rng};
            while (!functionQueue.push_back(object)) std::this_thread::yield();
            --obj;
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

    std::jthread writer{[&, rng = Obj::RNG{seed}]() mutable noexcept {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            if (bufferQueue.allocate_and_release(sizeof(Obj), [&](std::span<std::byte> buffer) noexcept {
                    std::construct_at(std::bit_cast<Obj *>(buffer.data()), rng);
                    return sizeof(Obj);
                }))
                --obj;
            else
                std::this_thread::yield();
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
    constexpr uint32_t object_count = 10'0000;

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
        fmt::print("\nFunction Queue test ....\n");
        constexpr size_t buffer_size = object_count * sizeof(Obj);
        auto cleanOffsetArray = std::make_unique<std::atomic<uint16_t>[]>(FunctionQueue::clean_array_size(buffer_size));
        FunctionQueue functionQueue{std::bit_cast<std::byte *>(buffer.get()), buffer_size, cleanOffsetArray.get()};
        test(functionQueue, num_threads, objects, seed);
    }

    {
        fmt::print("\nObject Queue test ....\n");
        ObjectQueue objectQueue{reinterpret_cast<Obj *>(buffer.get()), object_count};
        test(objectQueue, num_threads, objects, seed);
    }

    {
        fmt::print("\nBuffer Queue test ....\n");
        BufferQueue bufferQueue{std::bit_cast<std::byte *>(buffer.get()), object_count * sizeof(Obj)};
        test(bufferQueue, num_threads, objects, seed);
    }
}
