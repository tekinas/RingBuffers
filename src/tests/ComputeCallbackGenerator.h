#ifndef COMPUTECALLBACKGENERATOR
#define COMPUTECALLBACKGENERATOR

#include "util.h"
#include <boost/container_hash/hash.hpp>
#include <boost/container_hash/hash_fwd.hpp>
#include <boost/random/mersenne_twister.hpp>

#include <RingBuffers/FunctionWrapper.h>
#include <limits>

using util::Random;
using RNG = Random<boost::random::mt19937_64>;

inline size_t compute_1(size_t num) noexcept {
    boost::hash_combine(num, 2323442);
    boost::hash_combine(num, 1211113);
    boost::hash_combine(num, 34234235ul);

    return num;
}

inline size_t compute_2(size_t num) noexcept {
    boost::hash_combine(num, 24234235ul);
    boost::hash_combine(num, num);
    boost::hash_combine(num, num);
    return num;
}

inline size_t compute_3(size_t num) noexcept { return compute_1(compute_2(num)); }

template<size_t fields>
class ComputeFunctor {
private:
    size_t data[fields];

public:
    explicit ComputeFunctor(RNG &rng) noexcept {
        auto range = util::random_range<size_t>(rng);
        std::ranges::copy_n(range.begin(), std::size(data), std::begin(data));
    }

    size_t operator()(size_t num) const noexcept {
        boost::hash_range(num, std::begin(data), std::end(data));
        return num;
    }
};

template<size_t fields>
class ComputeFunctor2 {
private:
    size_t data[fields];
    uint16_t data2[fields];

public:
    explicit ComputeFunctor2(RNG &rng) noexcept {
        {
            auto range = util::random_range<size_t>(rng);
            std::ranges::copy_n(range.begin(), std::size(data), std::begin(data));
        }
        {
            auto range = util::random_range<uint16_t>(rng);
            std::ranges::copy_n(range.begin(), std::size(data2), std::begin(data2));
        }
    }

    size_t operator()(size_t num) const noexcept {
        boost::hash_range(num, std::begin(data), std::end(data));
        boost::hash_range(num, std::begin(data2), std::end(data2));
        return num;
    }
};

class CallbackGenerator {
private:
    RNG random;

public:
    explicit CallbackGenerator(size_t seed) noexcept : random{seed} {}

    void setSeed(uint32_t seed) { random.set_seed(seed); }

    template<typename T>
    void addCallback(T &&push_back) noexcept {
        switch (random.get_rand(0, 12)) {
            case 0: {
                auto constexpr max_ = std::numeric_limits<uint64_t>::max();
                auto a = random.get_rand<uint64_t>(0, max_);
                auto b = random.get_rand<uint64_t>(0, max_);
                auto c = random.get_rand<uint64_t>(0, max_);
                push_back([=](size_t num) noexcept {
                    boost::hash_combine(num, num);
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, b);
                    boost::hash_combine(num, c);
                    boost::hash_combine(num, num);
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, b);
                    boost::hash_combine(num, c);
                    boost::hash_combine(num, num);
                    return num;
                });
            } break;
            case 1: {
                auto constexpr max_ = std::numeric_limits<uint32_t>::max();
                auto a = random.get_rand<uint32_t>(0, max_);
                auto b = random.get_rand<uint32_t>(0, max_);
                auto c = random.get_rand<size_t>(0, max_);
                auto d = random.get_rand<size_t>(0, max_);
                auto e = random.get_rand<size_t>(0, max_);
                auto f = random.get_rand<size_t>(0, max_);
                auto g = random.get_rand<size_t>(0, max_);
                push_back([=](size_t num) noexcept {
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, b);
                    boost::hash_combine(num, c);
                    boost::hash_combine(num, d);
                    boost::hash_combine(num, e);
                    boost::hash_combine(num, f);
                    boost::hash_combine(num, g);
                    return num;
                });
            } break;
            case 2:
                push_back(rb::function<compute_1>);
                break;
            case 3:
                push_back(rb::function<compute_2>);
                break;
            case 4:
                push_back(rb::function<compute_3>);
                break;
            case 5: {
                push_back(ComputeFunctor<10>{random});
            } break;
            case 6: {
                push_back(ComputeFunctor2<10>{random});
            } break;
            case 7: {
                push_back(ComputeFunctor<7>{random});
            } break;
            case 8: {
                push_back(ComputeFunctor2<5>{random});
            } break;
            case 9: {
                push_back(ComputeFunctor<2>{random});
            } break;
            case 10: {
                push_back(ComputeFunctor<3>{random});
            } break;
            case 11: {
                push_back(
                        [a = random.get_rand<uint16_t>(0, std::numeric_limits<uint16_t>::max())](size_t num) noexcept {
                            boost::hash_combine(num, a);
                            boost::hash_combine(num, num);
                            boost::hash_combine(num, a);
                            boost::hash_combine(num, num);
                            return num;
                        });
            } break;
            case 12: {
                push_back([a = random.get_rand<uint8_t>(0, 255)](size_t num) noexcept {
                    boost::hash_combine(num, a);
                    return num;
                });
            } break;
        }
    }
};

#endif
