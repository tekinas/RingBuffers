#ifndef RB_DETAIL
#define RB_DETAIL

#include <atomic>
#include <bit>
#include <cstdint>
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

        friend bool operator==(TaggedUint32 const &l, TaggedUint32 const &r) noexcept {
            return l.value == r.value && l.tag == r.tag;
        }

        uint32_t getValue() const noexcept { return value; }

        uint32_t getTag() const noexcept { return tag; }

        TaggedUint32 getIncrTagged(uint32_t new_value) const noexcept { return {new_value, tag + 1}; }

        TaggedUint32 getSameTagged(uint32_t new_value) const noexcept { return {new_value, tag}; }

    private:
        TaggedUint32(uint32_t value, uint32_t index) noexcept : value{value}, tag{index} {}

        alignas(std::atomic<uint64_t>) uint32_t value{};
        uint32_t tag{};
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
            const uintptr_t fp_base = std::bit_cast<uintptr_t>(&fp_base_func) & uintptr_t{0XFFFFFFFF00000000lu};
            return std::bit_cast<FPtr>(fp_base + fp_offset)(static_cast<FunctionArgs>(fargs)...);
        }
    };

    template<typename T, size_t alignment = alignof(T)>
    static constexpr auto align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + alignment) & -alignment);
    }

}// namespace rb::detail

#endif
