#include "../BufferQueue_MCSP.h"
#include "../BufferQueue_SCSP.h"
#include "../FunctionQueue_MCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "../ObjectQueue_MCSP.h"
#include "../ObjectQueue_SCSP.h"
#include "util.h"

#include <atomic>
#include <bit>
#include <boost/container_hash/hash.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <thread>

using util::Random;
using util::StartFlag;
using util::Timer;

class Obj {
public:
    using RNG = Random<boost::random::mt19937_64>;
    Obj() noexcept = default;

    explicit Obj(RNG &rng) noexcept
        : a{rng.getRand<uint64_t>(std::numeric_limits<uint64_t>::min() + 1, std::numeric_limits<uint64_t>::max())},
          b{rng.getRand<float>(std::numeric_limits<float>::min(), std::numeric_limits<float>::max())},
          c{rng.getRand<uint32_t>(std::numeric_limits<uint32_t>::min(), std::numeric_limits<uint32_t>::max())} {}

    size_t operator()(Obj::RNG &rng, size_t seed) const noexcept {
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

using boost_queue = boost::lockfree::spsc_queue<Obj>;
using ObjectQueueSCSP = ObjectQueue_SCSP<Obj, false, false>;
using ObjectQueueMCSP = ObjectQueue_MCSP<Obj, false>;
using FunctionQueueSCSP =
        FunctionQueue_SCSP<size_t(Obj::RNG &, size_t), false, false, false, alignof(Obj) + sizeof(Obj)>;
using FunctionQueueMCSP = FunctionQueue_MCSP<size_t(Obj::RNG &, size_t), false, false>;
using BufferQueueSCSP = BufferQueue_SCSP<false, false, alignof(Obj)>;
using BufferQueueMCSP = BufferQueue_MCSP<false, alignof(Obj)>;

void test(boost_queue &objectQueue, uint32_t objects, size_t seed) noexcept {
    StartFlag start_flag;

    std::jthread writer{[&objectQueue, &start_flag, objects, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj--) {
            Obj o{rng};
            while (!objectQueue.push(o))
                ;
        }
    }};

    std::jthread reader{[&objectQueue, &start_flag, objects, seed, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};

            auto obj = objects;
            while (obj) {
                while (objectQueue.empty()) std::this_thread::yield();
                obj -= objectQueue.consume_all([&](Obj const &obj) { seed = obj(rng, seed); });
            }
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    start_flag.start();
}

template<typename ObjectQueue>
requires std::same_as<ObjectQueue, ObjectQueueSCSP> || std::same_as<ObjectQueue, ObjectQueueMCSP>
void test(ObjectQueue &objectQueue, uint32_t objects, size_t seed) noexcept {
    StartFlag start_flag;

    std::jthread writer{[&objectQueue, &start_flag, objects, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj--) {
            Obj o{rng};
            while (!objectQueue.push_back(o))
                ;
        }
    }};

    std::jthread reader{[&objectQueue, &start_flag, objects, seed, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};

            auto obj = objects;
            while (obj) {
                if constexpr (std::is_same_v<ObjectQueue, ObjectQueueSCSP>) {
                    while (!objectQueue.reserve()) std::this_thread::yield();
                    obj -= objectQueue.consume_all([&](Obj const &obj) { seed = obj(rng, seed); });
                } else {
                    auto const consumed = objectQueue.consume_all([&](Obj const &obj) { seed = obj(rng, seed); });
                    if (consumed) obj -= consumed;
                    else
                        std::this_thread::yield();
                }
            }
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    start_flag.start();
}

template<typename FunctionQueue>
requires std::same_as<FunctionQueue, FunctionQueueSCSP> || std::same_as<FunctionQueue, FunctionQueueMCSP>
void test(FunctionQueue &functionQueue, uint32_t objects, size_t seed) noexcept {
    StartFlag start_flag;

    std::jthread writer{[&functionQueue, &start_flag, objects, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            Obj o{rng};
            while (!functionQueue.push_back(o))
                ;
            --obj;
        }
    }};

    std::jthread reader{[&functionQueue, &start_flag, objects, seed, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};

            auto obj = objects;
            while (obj) {
                if constexpr (std::is_same_v<FunctionQueue, FunctionQueueSCSP>) {
                    if (functionQueue.reserve()) {
                        seed = functionQueue.call_and_pop(rng, seed);
                        --obj;
                    } else
                        std::this_thread::yield();
                } else {
                    if (auto handle = functionQueue.get_function_handle()) {
                        seed = handle.call_and_pop(rng, seed);
                        --obj;
                    } else
                        std::this_thread::yield();
                }
            }
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    start_flag.start();
}

template<typename BufferQueue>
requires std::same_as<BufferQueue, BufferQueueSCSP> || std::same_as<BufferQueue, BufferQueueMCSP>
void test(BufferQueue &bufferQueue, uint32_t objects, size_t seed) noexcept {
    StartFlag start_flag;

    std::jthread writer{[&bufferQueue, &start_flag, objects, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            Obj o{rng};
            while (!bufferQueue.allocate_and_release(sizeof(Obj), [&](std::span<std::byte> buffer) noexcept -> size_t {
                if (buffer.size() >= sizeof(Obj)) {
                    new (buffer.data()) Obj{o};
                    return sizeof(Obj);
                }
                return 0;
            }))
                ;
            --obj;
        }
    }};

    std::jthread reader{[&bufferQueue, &start_flag, objects, seed, rng = Obj::RNG{seed}]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};
            auto obj = objects;
            while (obj) {
                if constexpr (std::is_same_v<BufferQueue, BufferQueueSCSP>) {
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
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    start_flag.start();
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./oq_test_1r_1w <capacity> <seed> <objects>\n");

    size_t const capacity = [&] { return (argc >= 2) ? atol(argv[1]) : 100'000; }();
    fmt::print("capacity : {}\n", capacity);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : std::random_device{}(); }();
    fmt::print("seed : {}\n", seed);

    size_t const objects = [&] { return (argc >= 4) ? atol(argv[3]) : 100'000'000; }();
    fmt::print("objects : {}\n", objects);

    {
        boost_queue boostQueue{capacity};
        fmt::print("\nboost queue test ...\n");
        test(boostQueue, objects, seed);
    }

    auto buffer = std::make_unique<std::aligned_storage_t<sizeof(Obj), alignof(Obj)>[]>(capacity);

    {
        fmt::print("\nobject queue scsp test ...\n");
        ObjectQueueSCSP objectQueueSCSP{reinterpret_cast<Obj *>(buffer.get()), capacity};
        test(objectQueueSCSP, objects, seed);
    }

    {
        fmt::print("\nobject queue mcsp test ...\n");
        ObjectQueueMCSP objectQueueMCSP{reinterpret_cast<Obj *>(buffer.get()), capacity};
        test(objectQueueMCSP, objects, seed);
    }

    {
        fmt::print("\nbuffer queue scsp test ...\n");
        BufferQueueSCSP bufferQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test(bufferQueue, objects, seed);
    }

    {
        fmt::print("\nbuffer queue mcsp test ...\n");
        BufferQueueMCSP bufferQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test(bufferQueue, objects, seed);
    }

    {
        fmt::print("\nfunction queue scsp test ...\n");
        FunctionQueueSCSP funtionQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test(funtionQueue, objects, seed);
    }

    {
        fmt::print("\nfunction queue mcsp test ...\n");
        FunctionQueueMCSP funtionQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test(funtionQueue, objects, seed);
    }
}
