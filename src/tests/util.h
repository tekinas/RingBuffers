#ifndef UTIL
#define UTIL

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <memory_resource>
#include <random>
#include <span>
#include <type_traits>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include "uniform_distribution.h"

namespace util {
    template<typename RNG_Type = std::mt19937_64>
    class Random {
    public:
        Random() : rng{std::random_device{}()} {}

        template<std::integral T>
        Random(T t) : rng{t} {}

        template<std::integral T>
        void setSeed(T t) {
            rng.seed(t);
        }

        template<std::floating_point T = float>
        T getRand() {
            return util::uniform_real_distribution<T>{T{0.0}, T{1.0}}(rng);
        }

        template<typename T>
        requires std::integral<T> || std::floating_point<T> T getRand(T lower, T upper) {
            if constexpr (std::is_integral_v<T>) {
                return util::uniform_int_distribution<T>{lower, upper}(rng);
            } else if constexpr (std::is_floating_point_v<T>) {
                return util::uniform_real_distribution<T>{lower, upper}(rng);
            }
        }

        template<typename T, typename Container>
        requires std::integral<T> || std::floating_point<T>
        void addRand(T lower, T upper, uint64_t num, std::back_insert_iterator<Container> it) {
            auto dist = std::conditional_t<std::is_integral_v<T>, util::uniform_int_distribution<T>,
                                           util::uniform_real_distribution<T>>{lower, upper};
            while (num--) { it = dist(rng); }
        }

        template<typename T, typename Container>
        requires std::integral<T> || std::floating_point<T>
        void addRand(T lower, T upper, uint64_t num, std::insert_iterator<Container> it) {
            auto dist = std::conditional_t<std::is_integral_v<T>, util::uniform_int_distribution<T>,
                                           util::uniform_real_distribution<T>>{lower, upper};
            while (num--) { it = dist(rng); }
        }

        template<typename T>
        requires std::integral<T> || std::floating_point<T>
        void setRand(T lower, T upper, std::span<T> span) {
            auto dist = std::conditional_t<std::is_integral_v<T>, util::uniform_int_distribution<T>,
                                           util::uniform_real_distribution<T>>{lower, upper};
            for (auto &val : span) val = dist(rng);
        }

    private:
        RNG_Type rng;
    };

    template<typename T>
    class pmr_deleter {
    private:
        std::pmr::memory_resource *memoryResource;

    public:
        pmr_deleter() noexcept : memoryResource{nullptr} {}

        /*explicit*/ pmr_deleter(std::pmr::memory_resource *memoryResource) noexcept : memoryResource{memoryResource} {}

        void operator()(T *const ptr) const noexcept {
            ptr->~T();
            memoryResource->deallocate(ptr, sizeof(T), alignof(T));
        }
    };

    template<typename T>
    class pmr_deleter<T[]> {
    private:
        std::pmr::memory_resource *memoryResource;
        size_t size;

    public:
        pmr_deleter() noexcept : memoryResource{nullptr}, size{0} {}

        pmr_deleter(std::pmr::memory_resource *memoryResource, size_t size) noexcept
            : memoryResource{memoryResource}, size{size} {}

        [[nodiscard]] size_t getSize() const noexcept { return size; }

        [[nodiscard]] std::pmr::memory_resource *getResource() const noexcept { return memoryResource; }

        void operator()(T *const ptr) const noexcept {
            std::destroy_n(ptr, size);

            memoryResource->deallocate(ptr, size * sizeof(T), alignof(T));
        }
    };

}// namespace util

namespace std::pmr {
    template<typename T>
    using unique_ptr = std::unique_ptr<T, util::pmr_deleter<T>>;
}

namespace util {
    template<typename T>
    struct UniquePointerType {
        using single_object = std::pmr::unique_ptr<T>;
    };

    template<typename T>
    struct UniquePointerType<T[]> {
        using array = std::pmr::unique_ptr<T[]>;
    };

    template<typename T, size_t Bound>
    struct UniquePointerType<T[Bound]> {
        struct invalid_type {};
    };

    template<typename T, typename... ArgTypes>
    inline typename UniquePointerType<T>::single_object make_unique(std::pmr::memory_resource *memoryResource,
                                                                    ArgTypes &&...args) noexcept {
        std::pmr::polymorphic_allocator<T> alloc{memoryResource};
        auto ptr = alloc.allocate(1);
        alloc.construct(ptr, std::forward<ArgTypes>(args)...);

        return std::pmr::unique_ptr<T>{ptr, pmr_deleter<T>{memoryResource}};
    }

    template<typename T>
    inline typename UniquePointerType<T>::array make_unique(std::pmr::memory_resource *memoryResource,
                                                            size_t const size) noexcept {
        using type = std::remove_extent_t<T>;

        std::pmr::polymorphic_allocator<type> alloc{memoryResource};
        auto const ptr = alloc.allocate(size);

        for (size_t i = 0; i != size; ++i) { alloc.construct(ptr + i); }

        return std::pmr::unique_ptr<T>{ptr, pmr_deleter<T>{memoryResource, size}};
    }

    template<typename T>
    inline typename UniquePointerType<T>::invalid_type make_unique(std::pmr::memory_resource *memoryResource,
                                                                   size_t size) = delete;
}// namespace util

namespace util {
    class Timer {
    public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;
        using print_duration_t = std::chrono::duration<double>;

        explicit Timer(std::string_view str) noexcept : name{str}, start{clock::now()} {}

        ~Timer() noexcept {
            fmt::print("{} : {} s\n", name, std::chrono::duration_cast<print_duration_t>(clock::now() - start).count());
        }

    private:
        std::string const name;
        time_point const start;
    };
}// namespace util

namespace util {
    class StartFlag {
    public:
        void wait() const noexcept { m_StartFlag.wait(false, std::memory_order_acquire); }

        void start() noexcept {
            m_StartFlag.test_and_set(std::memory_order_release);
            m_StartFlag.notify_all();
        }

    private:
        std::atomic_flag m_StartFlag{false};
    };
}// namespace util

#endif
