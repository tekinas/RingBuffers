#ifndef RB_DETAIL
#define RB_DETAIL

#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>

namespace rb {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    class check_once_tag {};
    inline constexpr auto check_once = check_once_tag{};
};// namespace rb

namespace rb::detail {
    template<typename Function>
    class ScopeGaurd {
    public:
        template<typename F>
        ScopeGaurd(F &&func) noexcept : m_Func{std::forward<F>(func)} {}

        void release() noexcept { m_Commit = true; }

        ~ScopeGaurd() {
            if (!m_Commit) m_Func();
        }

    private:
        Function m_Func;
        bool m_Commit{false};
    };

    template<typename Func>
    ScopeGaurd(Func &&) -> ScopeGaurd<std::decay_t<Func>>;

    class TaggedUint32 {
    public:
        TaggedUint32() noexcept = default;

        bool operator==(TaggedUint32 const &r) const noexcept { return m_Tag == r.m_Tag && m_Value == r.m_Value; }

        uint32_t value() const noexcept { return m_Value; }

        uint32_t tag() const noexcept { return m_Tag; }

        TaggedUint32 incr_tagged(uint32_t new_value) const noexcept { return {m_Tag + 1, new_value}; }

        TaggedUint32 same_tagged(uint32_t new_value) const noexcept { return {m_Tag, new_value}; }

        static constexpr auto null() {
            return TaggedUint32{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
        }

        static constexpr auto max() { return TaggedUint32{0, std::numeric_limits<uint32_t>::max()}; }

    private:
        constexpr TaggedUint32(uint32_t tag_index, uint32_t value) noexcept : m_Tag{tag_index}, m_Value{value} {}

        alignas(std::atomic<uint64_t>) uint32_t m_Tag{};
        uint32_t m_Value{};
    };

    template<typename FunctionSignature>
    class FunctionPtr;

    inline void fp_base_func() {}

    template<typename ReturnType, typename... FunctionArgs>
    class FunctionPtr<ReturnType(FunctionArgs...)> {
    private:
        using FPtr = ReturnType (*)(FunctionArgs...) noexcept;

        uint32_t fp_offset;

    public:
        FunctionPtr(FPtr fp) noexcept : fp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(fp))} {}

        template<typename... FArgs>
        requires std::is_nothrow_invocable_r_v<ReturnType, FPtr, FArgs...>
        decltype(auto) operator()(FArgs &&...fargs) const noexcept {
            auto const fp = [&] {
                if constexpr (sizeof(FPtr) == sizeof(uint32_t)) return std::bit_cast<FPtr>(fp_offset);
                else {
                    auto const fp_base = std::bit_cast<uintptr_t>(&fp_base_func) & 0XFFFFFFFF00000000lu;
                    return std::bit_cast<FPtr>(fp_base + fp_offset);
                }
            }();
            return std::invoke(fp, std::forward<FArgs>(fargs)...);
        }
    };

    template<typename T, size_t alignment = alignof(T)>
    auto align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + alignment) & -alignment);
    }

}// namespace rb::detail

namespace rb::detail {
#ifdef __cpp_lib_hardware_interference_size
    constexpr auto hardware_constructive_interference_size = std::hardware_constructive_interference_size;
    constexpr auto hardware_destructive_interference_size = std::hardware_destructive_interference_size;
#else
    // 64 bytes on x86-64 │ L1_CACHE_BYTES │ L1_CACHE_SHIFT │ __cacheline_aligned │ ...
    constexpr std::size_t hardware_constructive_interference_size{64};
    constexpr std::size_t hardware_destructive_interference_size{64};
#endif// namespace rb::detail
}// namespace rb::detail

#endif
