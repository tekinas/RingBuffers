#ifndef RB_DETAIL
#define RB_DETAIL

#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

namespace rb {
    class check_once_tag {};
    constexpr auto check_once = check_once_tag{};
};// namespace rb

namespace rb::detail {
    template<typename Function>
    class ScopeGaurd {
    public:
        template<typename F>
        ScopeGaurd(F &&func) : m_Func{std::forward<F>(func)} {}

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

    template<typename T>
    class FunctionPtr;

    inline void fp_base_func() {}

    template<typename ReturnType, typename... FunctionArgs>
    class FunctionPtr<ReturnType(FunctionArgs...)> {
    private:
        using FPtr = ReturnType (*)(FunctionArgs...) noexcept;

        uint32_t fp_offset;

    public:
        FunctionPtr(FPtr fp) noexcept : fp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(fp))} {}

        ReturnType operator()(FunctionArgs... fargs) const noexcept {
            uintptr_t const fp_base = std::bit_cast<uintptr_t>(&fp_base_func) & uintptr_t{0XFFFFFFFF00000000lu};
            return std::invoke(std::bit_cast<FPtr>(fp_base + fp_offset), static_cast<FunctionArgs>(fargs)...);
        }
    };

    template<typename T, size_t alignment = alignof(T)>
    auto align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + alignment) & -alignment);
    }

}// namespace rb::detail

#endif
