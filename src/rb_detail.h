#ifndef RB_DETAIL
#define RB_DETAIL

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

}// namespace rb_detail

#endif
