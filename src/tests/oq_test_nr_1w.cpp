#include "../ObjectQueue_MCSP.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <boost/container_hash/hash.hpp>
#include <boost/container_hash/hash_fwd.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include <mutex>
#include <thread>
#include <vector>

using namespace util;

struct Obj {
    uint32_t a;
    float b;
    uint64_t c;

    Obj() noexcept = default;

    explicit Obj(Random<> &rng) noexcept
        : a{rng.getRand<uint32_t>(1, 99999 + 0b10101010101)}, b{rng.getRand<float>(-1.13242424f, 788978.0f)},
          c{rng.getRand<uint64_t>(0, 835454325463)} {}

    uint64_t hash(Random<> &rng) const noexcept {
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

using boost_queue = boost::lockfree::spsc_queue<Obj>;
using ObjectQueue = ObjectQueue_MCSP<Obj, false>;

//void test(boost_queue &objectQueue, uint32_t objects, std::size_t seed) noexcept {
//    std::jthread reader{[&objectQueue, objects] {
//        Timer timer{"boost queue read time "};
//
//        auto obj = objects;
//        std::size_t seed{0};
//        while (obj) {
//            while (objectQueue.empty()) std::this_thread::yield();
//
//            /*objectQueue.consume_one([&](Obj const &obj) {
//                    obj.hash(seed);
//                    });
//                    --obj;*/
//
//            obj -= objectQueue.consume_all([&](Obj const &obj) { obj.hash(seed); });
//        }
//
//        println("boost queue hash of ", objects, " objects : ", seed);
//    }};
//
//    std::jthread writer{[&objectQueue, objects, seed] {
//        Random<> rng{seed};
//
//        auto obj = objects;
//        while (obj--) {
//            Obj o{rng};
//            while (!objectQueue.push(o)) std::this_thread::yield();
//        }
//    }};
//}

void test(ObjectQueue &objectQueue, uint16_t threads, uint32_t objects_per_thread, std::size_t seed) noexcept {
    std::vector<uint64_t> final_result;
    std::mutex final_result_mutex;
    std::vector<std::jthread> reader_threads;

    for (uint16_t i = 0; i != threads; ++i)
        reader_threads.emplace_back([&objectQueue, i, objects_per_thread, &final_result, &final_result_mutex] {
            constexpr std::size_t seed = 0;
            Random<> rng{seed};

            std::vector<uint64_t> local_result;
            local_result.reserve(objects_per_thread);

            {
                Timer timer{"read time "};

                auto obj = objects_per_thread;
                while (obj) {
                    //                    auto const obj_ptr = objectQueue.consume();
                    //                    if (obj_ptr) {
                    //                        --obj;
                    //                        local_result.push_back(obj_ptr.get()->hash(rng));
                    //                    } else
                    //                        std::this_thread::yield();

                    //                    if (objectQueue.consume([&](Obj &obj) { local_result.push_back(obj.hash(rng)); })) --obj;
                    //                    else
                    //                        std::this_thread::yield();

                    auto const consumed = objectQueue.consume_n([&, rem = obj](Obj &obj) mutable {
                        local_result.push_back(obj.hash(rng));
                        return --rem;
                    });
                    if (consumed) obj -= consumed;
                    else
                        std::this_thread::yield();
                }
            }

            println("thread ", i, " finished.");
            std::scoped_lock lock{final_result_mutex};
            final_result.insert(final_result.end(), local_result.begin(), local_result.end());
        });

    std::jthread writer{[&objectQueue, objects_per_thread, threads, seed] {
        Random<> rng{seed};

        auto obj = objects_per_thread * threads;
        while (obj) {
            //            Obj o{rng};
            //           while (!objectQueue.push_back(o)) std::this_thread::yield();
            //          --obj;


            while (!objectQueue.emplace_back(rng)) std::this_thread::yield();
            --obj;


            //            uint32_t emplaced;
            //            while (!(emplaced = objectQueue.emplace_back_n([&, obj](Obj *obj_ptr, uint32_t count) {
            //               auto const to_construct = std::min(obj, count);
            //              for (uint32_t i = 0; i != to_construct; ++i) { std::construct_at(obj_ptr + i, rng); }
            //             return to_construct;
            //        })))
            //           std::this_thread::yield();
            //      obj -= emplaced;
        }

        println("writer thread finished, objects produced : ", threads * objects_per_thread);
    }};

    for (auto &thread : reader_threads) thread.join();
    std::sort(final_result.begin(), final_result.end());
    println("hash result : ", boost::hash_range(final_result.begin(), final_result.end()));
}


int main(int argc, char **argv) {
    constexpr uint32_t object_count = 10'0000;

    size_t const objects = [&] { return (argc >= 2) ? atol(argv[1]) : 10'000'000; }();
    println("objects per thread :", objects);

    size_t const num_threads = [&] { return (argc >= 3) ? atol(argv[2]) : std::thread::hardware_concurrency(); }();
    println("num threads :", num_threads);

    size_t const seed = [&] { return (argc >= 4) ? atol(argv[3]) : 100; }();
    println("using seed :", seed);

    auto buffer = std::make_unique<std::aligned_storage_t<sizeof(Obj), alignof(Obj)>[]>(object_count);
    ObjectQueue objectQueue{reinterpret_cast<Obj *>(buffer.get()), object_count};

    //    boost_queue boostQueue{object_count};

    //    test(boostQueue, objects, seed);
    test(objectQueue, num_threads, objects, seed);
}
