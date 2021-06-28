#include "../FunctionQueue_SCSP.h"
#include "../ObjectQueue_MCSP.h"
#include "../ObjectQueue_SCSP.h"
#include "util.h"

#include <boost/container_hash/hash.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <thread>

using util::Random;
using util::Timer;

struct Obj {
    uint32_t a;
    float b;
    uint64_t c;

    Obj() noexcept = default;

    explicit Obj(Random<> &rng) noexcept
        : a{rng.getRand<uint32_t>(1, 99999 + 0b10101010101)}, b{rng.getRand<float>(-1.13242424f, 788978.0f)},
          c{rng.getRand<uint64_t>(0, 835454325463)} {}

    void operator()(std::size_t &seed) const noexcept {
        boost::hash_combine(seed, a);
        boost::hash_combine(seed, b);
        boost::hash_combine(seed, c);
    }
};

bool OQ_IsObjectFree(Obj *ptr) noexcept {
    return reinterpret_cast<std::atomic<uint32_t> &>(ptr->a).load(std::memory_order_acquire) == 0;
}

void OQ_FreeObject(Obj *ptr) noexcept {
    reinterpret_cast<std::atomic<uint32_t> &>(ptr->a).store(0, std::memory_order_release);
}

using boost_queue = boost::lockfree::spsc_queue<Obj>;
using ObjectQueueSCSP = ObjectQueue_SCSP<Obj, false, false>;
using ObjectQueueMCSP = ObjectQueue_MCSP<Obj, false>;
using FunctionQueue = FunctionQueue_SCSP<void(size_t &), false, false>;

void test(boost_queue &objectQueue, uint32_t objects, std::size_t seed) noexcept {
    std::jthread reader{[&objectQueue, objects] {
        Timer timer{"read time "};

        auto obj = objects;
        std::size_t seed{0};
        while (obj) {
            while (objectQueue.empty()) std::this_thread::yield();

            obj -= objectQueue.consume_all([&](Obj const &obj) { obj(seed); });
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    std::jthread writer{[&objectQueue, objects, seed] {
        Random<> rng{seed};

        auto obj = objects;
        while (obj--) {
            Obj o{rng};
            while (!objectQueue.push(o)) std::this_thread::yield();
        }
    }};
}

void test(auto &objectQueue, uint32_t objects, std::size_t seed) noexcept {
    std::jthread reader{[&objectQueue, objects] {
        Timer timer{"read time "};

        auto obj = objects;
        std::size_t seed{0};
        while (obj) {
            auto consumed = objectQueue.consume_all([&](Obj const &obj) { obj(seed); });
            if (consumed) obj -= consumed;
            else
                std::this_thread::yield();
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    std::jthread writer{[&objectQueue, objects, seed] {
        Random<> rng{seed};

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
}

void test(FunctionQueue &functionQueue, uint32_t objects, std::size_t seed) noexcept {
    std::jthread reader{[&functionQueue, objects] {
        Timer timer{"read time "};

        auto obj = objects;
        std::size_t seed{0};
        while (obj) {
            while (!functionQueue.reserve()) std::this_thread::yield();
            functionQueue.call_and_pop(seed);
            --obj;
        }

        fmt::print("hash of {} objects : {}\n", objects, seed);
    }};

    std::jthread writer{[&functionQueue, objects, seed] {
        Random<> rng{seed};

        auto obj = objects;
        while (obj) {
            while (!functionQueue.emplace_back<Obj>(rng)) std::this_thread::yield();
            --obj;
        }
    }};
}

int main() {
    constexpr uint32_t object_count = 10'0000;
    constexpr uint32_t objects = 100'000'000;
    constexpr std::size_t seed = 121212121;

    auto buffer = std::make_unique<std::aligned_storage_t<sizeof(Obj), alignof(Obj)>[]>(object_count);
    ObjectQueueSCSP objectQueueSCSP{reinterpret_cast<Obj *>(buffer.get()), object_count};
    ObjectQueueMCSP objectQueueMCSP{reinterpret_cast<Obj *>(buffer.get()), object_count};
    FunctionQueue funtionQueue{reinterpret_cast<std::byte*>(buffer.get()),sizeof(Obj) * object_count};

    boost_queue boostQueue{object_count};

    fmt::print("boost queue test ...\n");
    test(boostQueue, objects, seed);

    fmt::print("\nobject queue scsp test ...\n");
    test(objectQueueSCSP, objects, seed);

    fmt::print("\nobject queue mcsp test ...\n");
    test(objectQueueMCSP, objects, seed);

    fmt::print("\nfunction queue test ...\n");
    test(funtionQueue, objects, seed);
}
