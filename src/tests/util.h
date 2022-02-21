#ifndef UTIL
#define UTIL

#include <atomic>
#include <boost/random.hpp>
#include <cmath>
#include <concepts>
#include <condition_variable>
#include <fmt/format.h>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <random>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>

namespace util {
    template<typename InputItrImpl>
    auto get_input_range(InputItrImpl itr) {
        struct InputItr : InputItrImpl {
            using InputItrImpl::done;
            using InputItrImpl::next;
            using InputItrImpl::value;

            using value_type = decltype(std::declval<InputItrImpl>().value());
            using difference_type = std::ptrdiff_t;

            value_type operator*() const noexcept { return value(); }
            auto &operator++() noexcept {
                next();
                return *this;
            }
            auto operator++(int) noexcept {
                auto temp = *this;
                ++*this;
                return temp;
            }
            bool operator==(int) const noexcept { return done(); }
        };

        return std::ranges::subrange{InputItr{itr}, int{}};
    }

    template<typename RNG_Type = std::mt19937_64>
    class Random {
    public:
        using result_type = typename RNG_Type::result_type;

        template<std::integral I>
        using IntDist = boost::random::uniform_int_distribution<I>;

        template<std::floating_point F>
        using RealDist = boost::random::uniform_real_distribution<F>;

        Random() noexcept : rng{std::random_device{}()} {}

        explicit Random(result_type seed) noexcept : rng{seed} {}

        void set_seed(result_type seed) noexcept { rng.seed(seed); }

        template<typename T>
        requires std::floating_point<T> || std::integral<T>
        auto get_rand() noexcept {
            using Limit = std::numeric_limits<T>;
            return get_rand(Limit::min(), Limit::max());
        }

        template<std::integral T>
        auto get_rand(T lower, T upper) noexcept {
            return IntDist<T>{lower, upper}(rng);
        }

        template<std::floating_point T>
        auto get_rand(T lower, T upper) noexcept {
            return RealDist<T>{lower, upper}(rng);
        }

    private:
        RNG_Type rng;
    };

    template<typename T, typename Random>
    requires std::integral<T> or std::floating_point<T>
    auto random_range(Random &random, T lower, T upper) noexcept {
        struct Iterator {
            Random *rng;
            T lower, upper, current{};
            T value() const noexcept { return current; }
            void next() noexcept { current = rng->get_rand(lower, upper); }
            bool done() const noexcept { return false; }
        };
        return get_input_range(Iterator{&random, lower, upper});
    }

    template<typename T, typename Random>
    requires std::integral<T> or std::floating_point<T>
    auto random_range(Random &random) noexcept {
        using Limit = std::numeric_limits<T>;
        return random_range(random, Limit::min(), Limit::max());
    }
}// namespace util


namespace util {
    template<typename T>
    requires(not std::is_bounded_array_v<T>) class pmr_deleter {
    private:
        static constexpr bool is_array_deleter{std::is_unbounded_array_v<T>};
        using object_type = std::remove_extent_t<T>;

    public:
        explicit pmr_deleter(std::pmr::memory_resource *memoryResource) noexcept : m_MemoryResource{memoryResource} {}

        explicit pmr_deleter(std::pmr::memory_resource *memoryResource, size_t size) noexcept requires is_array_deleter
            : m_MemoryResource{memoryResource},
              m_Size{size} {}

        size_t array_size() const noexcept requires is_array_deleter { return m_Size; }

        std::pmr::memory_resource *resource() const noexcept { return m_MemoryResource; }

        void operator()(object_type *ptr) const noexcept {
            std::pmr::polymorphic_allocator<> alloc{m_MemoryResource};
            if constexpr (is_array_deleter) {
                std::destroy_n(ptr, m_Size);
                alloc.deallocate_object(ptr, m_Size);
            } else
                alloc.delete_object(ptr);
        }

    private:
        class Empty {};
        std::pmr::memory_resource *m_MemoryResource;
        [[no_unique_address]] std::conditional_t<is_array_deleter, size_t, Empty> m_Size;
    };

    template<typename T>
    using pmr_unique_ptr = std::unique_ptr<T, pmr_deleter<T>>;

    template<typename T, typename... Args>
    requires(!std::is_array_v<T>) pmr_unique_ptr<T> make_unique(std::pmr::memory_resource *memoryResource,
                                                                Args &&...args)
    noexcept {
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        return {alloc.new_object<T>(std::forward<Args>(args)...), pmr_deleter<T>{memoryResource}};
    }

    template<typename T>
    requires(!std::is_array_v<T>) pmr_unique_ptr<T> make_unique_for_overwrite(std::pmr::memory_resource *memoryResource)
    noexcept {
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        auto const ptr = alloc.allocate_object<T>();
        return {::new (static_cast<void *>(ptr)) T, pmr_deleter<T>{memoryResource}};
    }

    template<class T>
    requires std::is_unbounded_array_v<T> pmr_unique_ptr<T> make_unique(std::pmr::memory_resource *memoryResource,
                                                                        size_t n)
    noexcept {
        using type = std::remove_extent_t<T>;
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        auto const ptr = alloc.allocate_object<type>(n);
        return {::new (static_cast<void *>(ptr)) type[n]{}, pmr_deleter<T>{memoryResource, n}};
    }

    template<class T>
    requires std::is_unbounded_array_v<T> pmr_unique_ptr<T>
    make_unique_for_overwrite(std::pmr::memory_resource *memoryResource, size_t n)
    noexcept {
        using type = std::remove_extent_t<T>;
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        auto const ptr = alloc.allocate_object<type>(n);
        return {::new (static_cast<void *>(ptr)) type[n], pmr_deleter<T>{memoryResource, n}};
    }

    template<class T, class... Args>
    requires std::is_bounded_array_v<T>
    void make_unique(Args &&...) = delete;

    template<class T, class... Args>
    requires std::is_bounded_array_v<T>
    void make_unique_for_overwrite(Args &&...) = delete;

    template<typename T>
    requires std::is_unbounded_array_v<T> std::span<T> get_span(pmr_unique_ptr<T> const &uptr)
    noexcept { return {uptr.get(), uptr.get_deleter().array_size()}; }

    template<typename T>
    requires std::is_unbounded_array_v<T>
    auto get_span(pmr_unique_ptr<T> const &&uptr) = delete;
}// namespace util

namespace util {
    class Timer {
    public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;
        using seconds = std::chrono::duration<double>;

        template<std::convertible_to<std::string> StrType>
        explicit Timer(StrType &&str) noexcept : name{std::forward<StrType>(str)}, start{clock::now()} {}

        Timer(Timer const &) = delete;
        Timer &operator=(Timer const &) = delete;

        ~Timer() {
            auto const duration = std::chrono::duration_cast<seconds>(clock::now() - start).count();
            fmt::print("{} : {} seconds\n", name, duration);
        }

    private:
        std::string name;
        time_point start;
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
        bool try_lock() noexcept {
            if (lock_flag.test(std::memory_order::relaxed)) return false;
            else
                return !lock_flag.test_and_set(std::memory_order::acquire);
        }

        void lock() noexcept {
            while (lock_flag.test_and_set(std::memory_order::acquire))
                while (lock_flag.test(std::memory_order::acquire)) std::this_thread::yield();
        }

        void unlock() noexcept { lock_flag.clear(std::memory_order::release); }

    private:
        std::atomic_flag lock_flag{};
    };
}// namespace util

namespace util {
    template<typename... Functors>
    constexpr auto overload(Functors &&...functors) noexcept {
        struct : std::remove_cvref_t<Functors>... {
            using std::remove_cvref_t<Functors>::operator()...;
        } overload_set{std::forward<Functors>(functors)...};
        return overload_set;
    }
}// namespace util

#endif
