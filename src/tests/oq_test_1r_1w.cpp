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
        : a{rng.getRand<uint32_t>(0, 99999 + 0b10101010101)}, b{rng.getRand<float>(-1.13242424f, 788978.0f)},
          c{rng.getRand<uint64_t>(0, 835454325463)} {}

    void hash(std::size_t &seed) const noexcept {
        boost::hash_combine(seed, a);
        boost::hash_combine(seed, b);
        boost::hash_combine(seed, c);
    }
};

using boost_queue = boost::lockfree::spsc_queue<Obj>;
using ObjectQueue = ObjectQueue_SCSP<Obj, false, false>;

void test(boost_queue &objectQueue, uint32_t objects, std::size_t seed) noexcept {
    std::jthread reader{[&objectQueue, objects] {
        Timer timer{"boost queue read time "};

        auto obj = objects;
        std::size_t seed{0};
        while (obj) {
            while (objectQueue.empty()) std::this_thread::yield();

            /*objectQueue.consume_one([&](Obj const &obj) {
                    obj.hash(seed);
                    });
                    --obj;*/

            obj -= objectQueue.consume_all([&](Obj const &obj) { obj.hash(seed); });
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

void test(ObjectQueue &objectQueue, uint32_t objects, std::size_t seed) noexcept {
    std::jthread reader{[&objectQueue, objects] {
        Timer timer{"ObjectQueue read time "};

        auto obj = objects;
        std::size_t seed{0};
        while (obj) {
            while (!objectQueue.reserve()) std::this_thread::yield();

            /*objectQueue.consume([&](Obj const &obj) {
                    obj.hash(seed);
                    });
                    --obj;*/

            /*objectQueue.consume().get()->hash(seed);
                    --obj;*/

            obj -= objectQueue.consume_all([&](Obj const &obj) { obj.hash(seed); });
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

int main() {
    constexpr uint32_t object_count = 10'0000;
    constexpr uint32_t objects = 100'000'000;
    constexpr std::size_t seed = 121212121;

    auto buffer = std::make_unique<std::aligned_storage_t<sizeof(Obj), alignof(Obj)>[]>(object_count);
    ObjectQueue objectQueue{reinterpret_cast<Obj *>(buffer.get()), object_count};

    boost_queue boostQueue{object_count};

    fmt::print("boost queue test ...\n");
    test(boostQueue, objects, seed);

    fmt::print("object queue test ...\n");
    test(objectQueue, objects, seed);
}
