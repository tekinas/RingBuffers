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

    void operator()(size_t &seed) const noexcept {
        boost::hash_combine(seed, a);
        boost::hash_combine(seed, b);
        boost::hash_combine(seed, c);
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
using FunctionQueueSCSP = FunctionQueue_SCSP<void(size_t &), false, false, false>;
using FunctionQueueMCSP = FunctionQueue_MCSP<void(size_t &), false, false>;
using BufferQueueSCSP = BufferQueue_SCSP<false, false, alignof(Obj)>;
using BufferQueueMCSP = BufferQueue_MCSP<false, alignof(Obj)>;

void test(boost_queue &objectQueue, uint32_t objects, size_t seed) noexcept {
    auto rng = Obj::RNG{seed};
    StartFlag start_flag;

    std::jthread writer{[&objectQueue, &rng, &start_flag, objects]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj--) {
            Obj o{rng};
            while (!objectQueue.push(o)) std::this_thread::yield();
        }
    }};

    std::jthread reader{[&objectQueue, &start_flag, objects, seed]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};

            auto obj = objects;
            while (obj) {
                while (objectQueue.empty()) std::this_thread::yield();
                obj -= objectQueue.consume_all([&](Obj const &obj) { obj(seed); });
            }
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    start_flag.start();
}

template<typename ObjectQueue>
requires std::same_as<ObjectQueue, ObjectQueueSCSP> || std::same_as<ObjectQueue, ObjectQueueMCSP>
void test(ObjectQueue &objectQueue, uint32_t objects, size_t seed) noexcept {
    auto rng = Obj::RNG{seed};
    StartFlag start_flag;

    std::jthread writer{[&objectQueue, &rng, &start_flag, objects]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            uint32_t emplaced;
            while (!(emplaced = objectQueue.emplace_back_n([&rng, obj](Obj *obj_ptr, uint32_t count) {
                auto const to_construct = std::min(obj, count);
                for (uint32_t i = 0; i != to_construct; ++i) { std::construct_at(obj_ptr + i, rng); }
                return to_construct;
            })))
                std::this_thread::yield();
            obj -= emplaced;
        }
    }};

    std::jthread reader{[&objectQueue, &start_flag, objects, seed]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};

            auto obj = objects;
            while (obj) {
                if constexpr (std::is_same_v<ObjectQueue, ObjectQueueSCSP>) {
                    while (!objectQueue.reserve()) std::this_thread::yield();
                    auto const consumed = objectQueue.consume_all([&](Obj const &obj) { obj(seed); });
                    obj -= consumed;
                } else {
                    auto const consumed = objectQueue.consume_all([&](Obj const &obj) { obj(seed); });
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
    auto rng = Obj::RNG{seed};
    StartFlag start_flag;

    std::jthread writer{[&functionQueue, &rng, &start_flag, objects]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            while (!functionQueue.template emplace_back<Obj>(rng)) std::this_thread::yield();
            --obj;
        }
    }};

    std::jthread reader{[&functionQueue, &start_flag, objects, seed]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};

            auto obj = objects;
            while (obj) {
                if constexpr (std::is_same_v<FunctionQueue, FunctionQueueSCSP>) {
                    while (!functionQueue.reserve()) std::this_thread::yield();
                    functionQueue.call_and_pop(seed);
                    --obj;
                } else {
                    if (auto handle = functionQueue.get_function_handle()) {
                        handle.call_and_pop(seed);
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
    auto rng = Obj::RNG{seed};
    StartFlag start_flag;

    std::jthread writer{[&bufferQueue, &rng, &start_flag, objects]() mutable {
        start_flag.wait();

        auto obj = objects;
        while (obj) {
            uint32_t emplaced{0};
            bufferQueue.allocate_and_release(sizeof(Obj), [&](std::span<std::byte> buffer) noexcept {
                emplaced = std::min(buffer.size() / sizeof(Obj), size_t{obj});
                auto base_addr = std::bit_cast<Obj *>(buffer.data());
                auto const end = base_addr + emplaced;
                for (; base_addr != end; ++base_addr) std::construct_at(base_addr, rng);

                return emplaced * sizeof(Obj);
            });

            if (emplaced) obj -= emplaced;
            else
                std::this_thread::yield();
        }
    }};

    std::jthread reader{[&bufferQueue, &start_flag, objects, seed]() mutable {
        start_flag.wait();

        {
            Timer timer{"read time "};
            auto getSpan = []<typename T>(std::span<std::byte> span) {
                return std::span{reinterpret_cast<T *>(span.data()), span.size() / sizeof(T)};
            };

            auto compute = [&seed, &getSpan](std::span<std::byte> buffer) {
                auto const obj_span = getSpan.template operator()<Obj>(buffer);
                for (auto &object : obj_span) object(seed);
                return obj_span.size();
            };

            auto obj = objects;
            while (obj) {
                if constexpr (std::is_same_v<BufferQueue, BufferQueueSCSP>) {
                    while (!bufferQueue.reserve()) std::this_thread::yield();
                    bufferQueue.consume_all([&](std::span<std::byte> buffer) { obj -= compute(buffer); });
                } else {
                    if (auto data_buffer = bufferQueue.consume()) {
                        obj -= compute(data_buffer.get());
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
