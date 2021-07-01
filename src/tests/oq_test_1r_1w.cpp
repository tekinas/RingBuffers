#include "../BufferQueue_SCSP.h"
#include "../FunctionQueue_SCSP.h"
#include "../ObjectQueue_MCSP.h"
#include "../ObjectQueue_SCSP.h"
#include "util.h"

#include <atomic>
#include <bit>
#include <boost/container_hash/hash.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <deque>
#include <span>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <thread>

using util::Random;
using util::Timer;

struct Obj {
    uint64_t a;
    float b;
    uint32_t c;

    Obj() noexcept = default;

    explicit Obj(Random<> &rng) noexcept
        : a{rng.getRand<uint64_t>(1, 835454325463)}, b{rng.getRand<float>(-1.13242424f, 788978.0f)},
          c{rng.getRand<uint32_t>(0, 99999 + 0b10101010101)} {}

    void operator()(size_t &seed) const noexcept {
        boost::hash_combine(seed, a);
        boost::hash_combine(seed, b);
        boost::hash_combine(seed, c);
    }
};

bool OQ_IsObjectFree(Obj *ptr) noexcept {
    return reinterpret_cast<std::atomic<uint64_t> &>(ptr->a).load(std::memory_order_acquire) == 0;
}

void OQ_FreeObject(Obj *ptr) noexcept {
    reinterpret_cast<std::atomic<uint64_t> &>(ptr->a).store(0, std::memory_order_release);
}

using boost_queue = boost::lockfree::spsc_queue<Obj>;
using ObjectQueueSCSP = ObjectQueue_SCSP<Obj, false, false>;
using ObjectQueueMCSP = ObjectQueue_MCSP<Obj, false>;
using FunctionQueue = FunctionQueue_SCSP<void(size_t &), false, false>;
using BufferQueueSCSP = BufferQueue_SCSP<false, false, alignof(Obj)>;

void test(boost_queue &objectQueue, uint32_t objects, size_t seed) noexcept {
    auto rng = Random<>{seed};
    auto spawn_read_thread = std::atomic_flag{};

    std::jthread writer{[&objectQueue, &rng, &spawn_read_thread, objects]() mutable {
        spawn_read_thread.test_and_set(std::memory_order_relaxed);
        spawn_read_thread.notify_one();

        auto obj = objects;
        while (obj--) {
            Obj o{rng};
            while (!objectQueue.push(o)) std::this_thread::yield();
        }
    }};

    spawn_read_thread.wait(false, std::memory_order::relaxed);

    std::jthread reader{[&objectQueue, objects] {
        Timer timer{"read time "};

        auto obj = objects;
        size_t seed{0};
        while (obj) {
            while (objectQueue.empty()) std::this_thread::yield();
            obj -= objectQueue.consume_all([&](Obj const &obj) { obj(seed); });
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};
}

void test(auto &objectQueue, uint32_t objects, size_t seed) noexcept {
    auto rng = Random<>{seed};
    auto spawn_read_thread = std::atomic_flag{};

    std::jthread writer{[&objectQueue, &rng, &spawn_read_thread, objects]() mutable {
        spawn_read_thread.test_and_set(std::memory_order_relaxed);
        spawn_read_thread.notify_one();

        auto obj = objects;
        while (obj) {
            /*Obj o{rng};
                    while (!objectQueue.push_back(o)) std::this_thread::yield();
       --obj;
       */

            /*while (!objectQueue.emplace_back(rng))
          std::this_thread::yield();
      --obj;*/

            uint32_t emplaced;
            while (!(emplaced = objectQueue.emplace_back_n([&, obj](Obj *obj_ptr, uint32_t count) {
                auto const to_construct = std::min(obj, count);
                for (uint32_t i = 0; i != to_construct; ++i) { std::construct_at(obj_ptr + i, rng); }
                return to_construct;
            })))
                std::this_thread::yield();
            obj -= emplaced;
        }
    }};

    spawn_read_thread.wait(false, std::memory_order::relaxed);

    std::jthread reader{[&objectQueue, objects] {
        Timer timer{"read time "};

        auto obj = objects;
        size_t seed{0};
        while (obj) {
            auto consumed = objectQueue.consume_all([&](Obj const &obj) { obj(seed); });
            if (consumed) obj -= consumed;
            else
                std::this_thread::yield();
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};
}

void test(FunctionQueue &functionQueue, uint32_t objects, size_t seed) noexcept {
    auto rng = Random<>{seed};
    auto spawn_read_thread = std::atomic_flag{};

    std::jthread writer{[&functionQueue, &rng, &spawn_read_thread, objects]() mutable {
        spawn_read_thread.test_and_set(std::memory_order_relaxed);
        spawn_read_thread.notify_one();

        auto obj = objects;
        while (obj) {
            while (!functionQueue.emplace_back<Obj>(rng)) std::this_thread::yield();
            --obj;
        }
    }};

    spawn_read_thread.wait(false, std::memory_order::relaxed);

    std::jthread reader{[&functionQueue, objects] {
        Timer timer{"read time "};

        auto obj = objects;
        size_t seed{0};
        while (obj) {
            while (!functionQueue.reserve()) std::this_thread::yield();
            functionQueue.call_and_pop(seed);
            --obj;
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};
}

void test(BufferQueueSCSP &bufferQueue, uint32_t objects, size_t seed) noexcept {
    auto rng = Random<>{seed};
    auto spawn_read_thread = std::atomic_flag{};

    std::jthread writer{[&bufferQueue, &rng, &spawn_read_thread, objects]() mutable {
        spawn_read_thread.test_and_set(std::memory_order_relaxed);
        spawn_read_thread.notify_one();

        auto obj = objects;
        while (obj) {
            uint32_t emplaced{0};
            bufferQueue.allocate_and_release(sizeof(Obj), [&](std::byte *buffer, uint32_t avl_size) noexcept {
                emplaced = std::min(avl_size / sizeof(Obj), size_t{obj});
                auto base_addr = std::bit_cast<Obj *>(buffer);
                auto const end = base_addr + emplaced;
                for (; base_addr != end; ++base_addr) std::construct_at(base_addr, rng);

                return emplaced * sizeof(Obj);
            });

            if (emplaced) obj -= emplaced;
            else
                std::this_thread::yield();
        }
    }};

    spawn_read_thread.wait(false, std::memory_order::relaxed);

    std::jthread reader{[&bufferQueue, objects] {
        Timer timer{"read time "};

        auto obj = objects;
        size_t seed{0};
        while (obj) {
            while (!bufferQueue.reserve()) std::this_thread::yield();
            bufferQueue.consume([&](std::byte *buffer, uint32_t size) {
                std::span span{std::bit_cast<Obj *>(buffer), size / sizeof(Obj)};
                for (auto &object : span) object(seed);
                obj -= span.size();
            });
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./oq_test_1r_1w <capacity> <seed> <objects>\n");

    size_t const capacity = [&] { return (argc >= 2) ? atol(argv[1]) : 100'000; }();
    fmt::print("capacity : {}\n", capacity);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
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
        fmt::print("\nfunction queue test ...\n");
        FunctionQueue funtionQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test(funtionQueue, objects, seed);
    }

    {
        fmt::print("\nbuffer queue test ...\n");
        BufferQueueSCSP bufferQueue{reinterpret_cast<std::byte *>(buffer.get()), sizeof(Obj) * capacity};
        test(bufferQueue, objects, seed);
    }
}
