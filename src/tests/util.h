#ifndef UTIL
#define UTIL

#include <atomic>
#include <boost/random.hpp>
#include <concepts>
#include <condition_variable>
#include <fmt/format.h>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <random>
#include <span>
#include <tuple>
#include <type_traits>

namespace util {
    template<typename RNG_Type = std::mt19937_64>
    class Random {
    public:
        using result_type = typename RNG_Type::result_type;

        Random() noexcept : rng{std::random_device{}()} {}

        explicit Random(result_type seed) noexcept : rng{seed} {}

        void setSeed(result_type seed) noexcept { rng.seed(seed); }

        template<typename T>
        requires std::floating_point<T> || std::integral<T> T getRand()
        noexcept {
            using Limit = std::numeric_limits<T>;
            auto dist = std::conditional_t<std::integral<T>, boost::random::uniform_int_distribution<T>,
                                           boost::random::uniform_real_distribution<T>>{Limit::min(), Limit::max()};
            return dist(rng);
        }

        template<typename T>
        requires std::integral<T> || std::floating_point<T> T getRand(T lower, T upper)
        noexcept {
            auto dist = std::conditional_t<std::integral<T>, boost::random::uniform_int_distribution<T>,
                                           boost::random::uniform_real_distribution<T>>{lower, upper};
            return dist(rng);
        }

        template<typename T>
        requires std::integral<T> || std::floating_point<T>
        void addRand(T lower, T upper, size_t num, std::output_iterator<T> auto itr) noexcept {
            auto dist = std::conditional_t<std::integral<T>, boost::random::uniform_int_distribution<T>,
                                           boost::random::uniform_real_distribution<T>>{lower, upper};
            while (num--) *itr++ = dist(rng);
        }

        template<typename T>
        requires std::integral<T> || std::floating_point<T>
        void setRand(T lower, T upper, std::span<T> span) noexcept {
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
    private:
        static_assert(!std::is_bounded_array_v<T>);

        static constexpr bool is_array_deleter{std::is_unbounded_array_v<T>};
        using object_type = std::remove_extent_t<T>;

    public:
        explicit pmr_deleter(std::pmr::memory_resource *memoryResource) noexcept : m_MemoryResource{memoryResource} {}

        pmr_deleter(std::pmr::memory_resource *memoryResource, size_t size) noexcept
            : m_MemoryResource{memoryResource}, m_Size{size} {}

        size_t array_size() const noexcept { return m_Size; }

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
        using deleter = pmr_deleter<T>;
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        return {alloc.new_object<T>(std::forward<Args>(args)...), deleter{memoryResource}};
    }

    template<typename T>
    requires(!std::is_array_v<T>) pmr_unique_ptr<T> make_unique_for_overwrite(std::pmr::memory_resource *memoryResource)
    noexcept {
        using deleter = pmr_deleter<T>;
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        auto const ptr{alloc.allocate_object<T>()};
        return {::new (static_cast<void *>(ptr)) T, deleter{memoryResource}};
    }

    template<class T>
    requires std::is_unbounded_array_v<T> pmr_unique_ptr<T> make_unique(std::pmr::memory_resource *memoryResource,
                                                                        size_t n)
    noexcept {
        using deleter = pmr_deleter<T>;
        using type = std::remove_extent_t<T>;
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        auto const ptr{alloc.allocate_object<type>(n)};
        return {::new (static_cast<void *>(ptr)) type[n]{}, deleter{memoryResource, n}};
    }

    template<class T>
    requires std::is_unbounded_array_v<T> pmr_unique_ptr<T>
    make_unique_for_overwrite(std::pmr::memory_resource *memoryResource, size_t n)
    noexcept {
        using deleter = pmr_deleter<T>;
        using type = std::remove_extent_t<T>;
        std::pmr::polymorphic_allocator<> alloc{memoryResource};
        auto const ptr{alloc.allocate_object<type>(n)};
        return {::new (static_cast<void *>(ptr)) type[n], deleter{memoryResource, n}};
    }

    template<class T, class... Args>
    requires std::is_bounded_array_v<T>
    void make_unique(Args &&...) = delete;

    template<class T, class... Args>
    requires std::is_bounded_array_v<T>
    void make_unique_for_overwrite(Args &&...) = delete;

    template<typename T>
    requires std::is_unbounded_array_v<T>
    auto get_span(pmr_unique_ptr<T> const &uptr) noexcept {
        return std::span{uptr.get(), uptr.get_deleter().array_size()};
    }

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

        ~Timer() {
            auto const duration{std::chrono::duration_cast<seconds>(clock::now() - start).count()};
            fmt::print("{} : {} seconds\n", name, duration);
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
    template<typename... Functors>
    auto overload(Functors &&...functors) noexcept {
        struct : std::remove_reference_t<Functors>... {
            using std::remove_reference_t<Functors>::operator()...;
        } overload_set{std::forward<Functors>(functors)...};
        return overload_set;
    }
}// namespace util
#endif
