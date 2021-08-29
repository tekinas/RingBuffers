#ifndef UTIL
#define UTIL

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <memory>
#include <memory_resource>
#include <random>
#include <span>
#include <type_traits>

#include <fmt/format.h>

#include <boost/random.hpp>

namespace util {
    template<typename RNG_Type = std::mt19937_64>
    class Random {
    public:
        Random() : rng{std::random_device{}()} {}

        template<std::unsigned_integral T>
        Random(T t) : rng{t} {}

        template<std::unsigned_integral T>
        void setSeed(T t) {
            rng.seed(t);
        }

        template<std::floating_point T = float>
        T getRand() {
            return boost::random::uniform_real_distribution<T>{T{0.0}, T{1.0}}(rng);
        }

        template<typename T>
        requires std::integral<T> || std::floating_point<T> T getRand(T lower, T upper) {
            if constexpr (std::is_integral_v<T>) {
                return boost::random::uniform_int_distribution<T>{lower, upper}(rng);
            } else if constexpr (std::is_floating_point_v<T>) {
                return boost::random::uniform_real_distribution<T>{lower, upper}(rng);
            }
        }

        template<typename T, typename Container>
        requires std::integral<T> || std::floating_point<T>
        void addRand(T lower, T upper, uint64_t num, std::back_insert_iterator<Container> it) {
            auto dist = std::conditional_t<std::is_integral_v<T>, boost::random::uniform_int_distribution<T>,
                                           boost::random::uniform_real_distribution<T>>{lower, upper};
            while (num--) { it = dist(rng); }
        }

        template<typename T, typename Container>
        requires std::integral<T> || std::floating_point<T>
        void addRand(T lower, T upper, uint64_t num, std::insert_iterator<Container> it) {
            auto dist = std::conditional_t<std::is_integral_v<T>, boost::random::uniform_int_distribution<T>,
                                           boost::random::uniform_real_distribution<T>>{lower, upper};
            while (num--) { it = dist(rng); }
        }

        template<typename T>
        requires std::integral<T> || std::floating_point<T>
        void setRand(T lower, T upper, std::span<T> span) {
            auto dist = std::conditional_t<std::is_integral_v<T>, boost::random::uniform_int_distribution<T>,
                                           boost::random::uniform_real_distribution<T>>{lower, upper};
            for (auto &val : span) val = dist(rng);
        }

    private:
        RNG_Type rng;
    };
}// namespace util


namespace util {
    template<typename T>
    class pmr_deleter {
    public:
        pmr_deleter(std::pmr::memory_resource *memoryResource) noexcept : memoryResource{memoryResource} {}

        void operator()(T *const ptr) const noexcept {
            if (ptr) std::destroy_at(ptr);
            memoryResource->deallocate(ptr, sizeof(T), alignof(T));
        }

    private:
        std::pmr::memory_resource *memoryResource;
    };

    template<typename T>
    class pmr_deleter<T[]> {
    public:
        pmr_deleter(std::pmr::memory_resource *memoryResource, size_t size) noexcept
            : memoryResource{memoryResource}, size{size} {}

        size_t getSize() const noexcept { return size; }

        std::pmr::memory_resource *getResource() const noexcept { return memoryResource; }

        void operator()(T *const ptr) const noexcept {
            if (ptr) std::destroy_n(ptr, size);
            memoryResource->deallocate(ptr, size * sizeof(T), alignof(T));
        }

    private:
        std::pmr::memory_resource *memoryResource;
        size_t size;
    };

    template<typename T>
    using pmr_unique_ptr = std::unique_ptr<T, pmr_deleter<T>>;

    template<typename T>
    struct UniquePointerType {
        using single_object = pmr_unique_ptr<T>;
    };

    template<typename T>
    struct UniquePointerType<T[]> {
        using array = pmr_unique_ptr<T[]>;
    };

    template<typename T, size_t Bound>
    struct UniquePointerType<T[Bound]> {
        struct invalid_type {};
    };

    template<typename T, typename... ArgTypes>
    typename UniquePointerType<T>::single_object make_unique(std::pmr::memory_resource *memoryResource,
                                                             ArgTypes &&...args) noexcept {
        std::pmr::polymorphic_allocator<T> alloc{memoryResource};
        auto const ptr = alloc.allocate(1);

        if (!ptr) return pmr_unique_ptr<T>{nullptr, pmr_deleter<T>{memoryResource}};

        alloc.construct(ptr, std::forward<ArgTypes>(args)...);
        return pmr_unique_ptr<T>{ptr, pmr_deleter<T>{memoryResource}};
    }

    template<typename T>
    typename UniquePointerType<T>::array make_unique(std::pmr::memory_resource *memoryResource,
                                                     size_t const size) noexcept {
        using type = std::remove_extent_t<T>;

        std::pmr::polymorphic_allocator<type> alloc{memoryResource};
        auto const ptr = alloc.allocate(size);

        if (!ptr) return pmr_unique_ptr<T>{nullptr, pmr_deleter<T>{memoryResource, size}};

        for (size_t i = 0; i != size; ++i) alloc.construct(ptr + i);
        return pmr_unique_ptr<T>{ptr, pmr_deleter<T>{memoryResource, size}};
    }

    template<typename T>
    typename UniquePointerType<T>::invalid_type make_unique(std::pmr::memory_resource *memoryResource,
                                                            size_t size) = delete;
}// namespace util

namespace util {
    class Timer {
    public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;
        using print_duration_t = std::chrono::duration<double>;

        explicit Timer(std::string_view str) noexcept : name{str}, start{clock::now()} {}

        ~Timer() {
            fmt::print("{} : {} seconds\n", name,
                       std::chrono::duration_cast<print_duration_t>(clock::now() - start).count());
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
        std::atomic_flag m_StartFlag{};
    };
}// namespace util

namespace util {
    class SpinLock {
    public:
        SpinLock() = default;

        bool try_lock() noexcept {
            if (lock_flag.test(std::memory_order::relaxed)) return false;
            else
                return !lock_flag.test_and_set(std::memory_order::acquire);
        }

        void lock() noexcept {
            while (lock_flag.test_and_set(std::memory_order::acquire)) std::this_thread::yield();
        }

        void unlock() noexcept { lock_flag.clear(std::memory_order::release); }

    private:
        std::atomic_flag lock_flag{};
    };
}// namespace util

namespace util {
    template<typename... Bases>
    class overload : public Bases... {
    public:
        using Bases::operator()...;

        template<typename... Obj>
        explicit overload(Obj &&...obj) : Bases{std::forward<Obj>(obj)}... {}
    };

    template<class... Bases>
    overload(Bases...) -> overload<Bases...>;
}// namespace util
#endif
