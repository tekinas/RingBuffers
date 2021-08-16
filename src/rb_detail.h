#ifndef RB_DETAIL
#define RB_DETAIL

#include <atomic>
#include <cstdint>
#include <utility>

namespace rb_detail {
    template<typename Function>
    class ScopeGaurd {
    public:
        template<typename F>
        ScopeGaurd(F &&func) : m_Func{std::forward<F>(func)} {}

        void commit() noexcept { m_Commit = true; }

        ~ScopeGaurd() {
            if (!m_Commit) m_Func();
        }

    private:
        Function m_Func;
        bool m_Commit{false};
    };

    template<typename Func>
    ScopeGaurd(Func &&) -> ScopeGaurd<std::decay_t<Func>>;

    class Empty {
    public:
        template<typename... T>
        explicit Empty(T &&...) noexcept {}
    };

    template<typename>
    class Type {};

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

}// namespace rb_detail

#endif
