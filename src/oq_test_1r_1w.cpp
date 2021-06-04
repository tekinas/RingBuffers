#include "ObjectQueue_SCSP.h"
#include "util.h"

#include <boost/container_hash/hash.hpp>
#include <thread>

using namespace util;

struct Obj {
    uint32_t a;
    float b;
    uint64_t c;

    explicit Obj(Random<> &rng) noexcept: a{rng.getRand<uint32_t>(0, 99999 + 0b10101010101)},
                                          b{rng.getRand<float>(-1.13242424f, 788978.0f)},
                                          c{rng.getRand<uint64_t>(0, 835454325463)} {}

    void hash(std::size_t &seed) const noexcept {
        boost::hash_combine(seed, a);
        boost::hash_combine(seed, b);
        boost::hash_combine(seed, c);
    }
};

int main() {
    using ObjectQueue = ObjectQueue_SCSP<Obj, false, false>;
    constexpr uint32_t object_count = 10'000;
    constexpr uint32_t objects = 100'000'000;

    auto buffer = std::make_unique<std::aligned_storage_t<sizeof(Obj), alignof(Obj)>[]>(object_count);
    ObjectQueue objectQueue{buffer.get(), object_count};

    std::jthread reader{[&objectQueue, objects] {
        Timer timer{"reader time "};

        auto obj = objects;
        std::size_t seed{0};
        while (obj) {
            while (!objectQueue.reserve()) std::this_thread::yield();

            /*objectQueue.consume([&](Obj const &obj) {
                obj.hash(seed);
            });
            --obj;*/

            obj -= objectQueue.consume_all([&](Obj const &obj) {
                obj.hash(seed);
            });
        }

        println("hash of ", objects, " objects : ", seed);
    }};

    std::jthread writer{[&objectQueue] {
        constexpr std::size_t seed = 1212121;
        Random<> rng{seed};

        auto obj = objects;
        while (obj--) {
//            Obj o{rng};
//            while (!objectQueue.push_back(o)) std::this_thread::yield();
            while (!objectQueue.emplace_back(rng)) std::this_thread::yield();
        }
    }};
}
