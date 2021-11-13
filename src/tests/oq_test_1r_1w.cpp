#include "RingBuffers/detail/rb_detail.h"
#include "util.h"
#include <RingBuffers/BufferQueue_MCSP.h>
#include <RingBuffers/BufferQueue_SCSP.h>
#include <RingBuffers/FunctionQueue_MCSP.h>
#include <RingBuffers/FunctionQueue_SCSP.h>
#include <RingBuffers/ObjectQueue_MCSP.h>
#include <RingBuffers/ObjectQueue_SCSP.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <limits>
#include <span>
#include <thread>
#include <type_traits>

#include <boost/container_hash/hash.hpp>
#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <fmt/format.h>


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

    size_t operator()(Obj::RNG &rng, size_t seed) const noexcept {
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

using namespace rb;
using BoostQueueSCSP = boost::lockfree::spsc_queue<Obj, boost::lockfree::fixed_sized<true>>;
using BoostQueueMCMP = boost::lockfree::queue<Obj, boost::lockfree::fixed_sized<true>>;
using ObjectQueueSCSP = ObjectQueue_SCSP<Obj>;
using ObjectQueueMCSP = ObjectQueue_MCSP<Obj, 1>;
using FunctionQueueSCSP = FunctionQueue_SCSP<size_t(Obj::RNG &, size_t), false, memory_footprint<Obj>>;
using FunctionQueueMCSP = FunctionQueue_MCSP<size_t(Obj::RNG &, size_t), 1, false, memory_footprint<Obj>>;
using BufferQueueSCSP = BufferQueue_SCSP<false, false, alignof(Obj)>;
using BufferQueueMCSP = BufferQueue_MCSP<false, alignof(Obj)>;

template<typename ObjectQueueType>
requires std::same_as<ObjectQueueType, BoostQueueSCSP> || std::same_as<ObjectQueueType, BoostQueueMCMP> ||
        std::same_as<ObjectQueueType, ObjectQueueSCSP> || std::same_as<ObjectQueueType, ObjectQueueMCSP> ||
        std::same_as<ObjectQueueType, FunctionQueueSCSP> || std::same_as<ObjectQueueType, FunctionQueueMCSP>
auto test(ObjectQueueType &objectQueue, size_t objects, size_t seed) noexcept {
    StartFlag start_flag;
    std::jthread writer{[&objectQueue, &start_flag, objects, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();
        for (auto obj{objects}; obj--;) {
            Obj o{rng};
            while (!objectQueue.push(o)) {
                if constexpr (std::same_as<ObjectQueueType, FunctionQueueMCSP> ||
                              std::same_as<ObjectQueueType, ObjectQueueMCSP>)
                    objectQueue.clean_memory();
            }
        }
    }};
    std::jthread reader{[&objectQueue, &start_flag, objects, seed, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();
        auto obj = objects;
        for (Timer timer{"read time "}; obj;) {
            if constexpr (std::same_as<ObjectQueueType, ObjectQueueSCSP>) {
                if (!objectQueue.empty())
                    obj -= objectQueue.consume_all([&](Obj const &obj) noexcept { seed = obj(rng, seed); });
            } else if constexpr (std::same_as<ObjectQueueType, ObjectQueueMCSP>) {
                auto const reader = objectQueue.get_reader(0);
                auto const consumed = reader.consume_all([&](Obj const &obj) noexcept { seed = obj(rng, seed); });
                if (consumed) obj -= consumed;
            } else if constexpr (std::same_as<ObjectQueueType, FunctionQueueSCSP>) {
                while (!objectQueue.empty()) {
                    seed = objectQueue.call_and_pop(rng, seed);
                    --obj;
                }
            } else if constexpr (std::same_as<ObjectQueueType, FunctionQueueMCSP>) {
                auto const reader = objectQueue.get_reader(0);
                for (FunctionQueueMCSP::FunctionHandle handle; (handle = reader.get_function_handle()); --obj)
                    seed = handle.call_and_pop(rng, seed);
            } else {
                if (!objectQueue.empty())
                    obj -= objectQueue.consume_all([&](Obj const &obj) noexcept { seed = obj(rng, seed); });
            }
            std::this_thread::yield();
        }
        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};
    start_flag.start();
    writer.join();
    reader.join();
    return seed;
}

template<typename BufferQueue>
requires std::same_as<BufferQueue, BufferQueueSCSP> || std::same_as<BufferQueue, BufferQueueMCSP>
auto test(BufferQueue &bufferQueue, size_t objects, size_t seed) noexcept {
    StartFlag start_flag;
    std::jthread writer{[&bufferQueue, &start_flag, objects, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();
        for (auto obj = objects; obj--;) {
            Obj o{rng};
            while (!bufferQueue.allocate_and_release(sizeof(Obj), [&](std::span<std::byte> buffer) noexcept -> size_t {
                if (buffer.size() >= sizeof(Obj)) {
                    new (buffer.data()) Obj{o};
                    return sizeof(Obj);
                }
                return 0;
            }))
                ;
        }
    }};
    std::jthread reader{[&bufferQueue, &start_flag, objects, seed, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();
        auto obj = objects;
        for (Timer timer{"read time "}; obj;) {
            if constexpr (std::same_as<BufferQueue, BufferQueueSCSP>) {
                while (!bufferQueue.reserve()) std::this_thread::yield();
                obj -= bufferQueue.consume_all([&](std::span<std::byte> buffer) {
                    seed = (*std::bit_cast<Obj *>(buffer.data())) (rng, seed);
                }) / sizeof(Obj);
            } else {
                if (auto data_buffer = bufferQueue.consume()) {
                    seed = (*std::bit_cast<Obj *>(data_buffer.get().data())) (rng, seed);
                    --obj;
                } else
                    std::this_thread::yield();
            }
        }
        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};
    start_flag.start();
    writer.join();
    reader.join();
    return seed;
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./oq_test_1r_1w <seed> <objects>\n");

    constexpr uint32_t capacity = 65'534;

    size_t const seed = [&] { return (argc >= 2) ? atol(argv[1]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);

    size_t const objects = [&] { return (argc >= 3) ? atol(argv[2]) : 2'000'000; }();
    fmt::print("objects : {}\n", objects);

    std::vector<size_t> test_results;
    {
        BoostQueueSCSP boostQueue{capacity};
        fmt::print("\nboost queue scsp test ...\n");
        test_results.push_back(test(boostQueue, objects, seed));
    }

    {
        BoostQueueMCMP boostQueue{capacity};
        fmt::print("\nboost queue mcmp test ...\n");
        test_results.push_back(test(boostQueue, objects, seed));
    }

    {
        fmt::print("\nobject queue scsp test ...\n");
        ObjectQueueSCSP objectQueueSCSP{capacity};
        test_results.push_back(test(objectQueueSCSP, objects, seed));
    }

    {
        fmt::print("\nobject queue mcsp test ...\n");
        ObjectQueueMCSP objectQueueMCSP{capacity, 1};
        test_results.push_back(test(objectQueueMCSP, objects, seed));
    }

    {
        fmt::print("\nbuffer queue scsp test ...\n");
        auto const buffer = std::make_unique<Obj[]>(capacity);
        BufferQueueSCSP bufferQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test_results.push_back(test(bufferQueue, objects, seed));
    }

    {
        fmt::print("\nbuffer queue mcsp test ...\n");
        auto const buffer = std::make_unique<Obj[]>(capacity);
        BufferQueueMCSP bufferQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test_results.push_back(test(bufferQueue, objects, seed));
    }

    {
        fmt::print("\nfunction queue scsp test ...\n");
        FunctionQueueSCSP funtionQueue{sizeof(Obj) * capacity};
        test_results.push_back(test(funtionQueue, objects, seed));
    }

    {
        fmt::print("\nfunction queue mcsp test ...\n");
        FunctionQueueMCSP funtionQueue{sizeof(Obj) * capacity, 1};
        test_results.push_back(test(funtionQueue, objects, seed));
    }

    if (!std::ranges::all_of(test_results, [val{test_results[0]}](auto elem) { return elem == val; })) {
        fmt::print("error : test results are not same");
        return EXIT_FAILURE;
    }
}
