#ifndef FUNCTION_WRAPPER
#define FUNCTION_WRAPPER

#include <concepts>
#include <type_traits>
#include <utility>

namespace rb::detail {
    template<typename T>
    concept function_ptr = std::is_function_v<std::remove_pointer_t<T>>;

    template<function_ptr auto func>
    class FunctionWrapper {
    public:
        template<typename... Args>
        requires std::is_invocable_v<decltype(func), Args...>
        decltype(auto) operator()(Args &&...args) const noexcept(std::is_nothrow_invocable_v<decltype(func), Args...>) {
            return func(std::forward<Args>(args)...);
        }
    };
}// namespace rb::detail

namespace rb {
    template<auto func>
    constexpr inline detail::FunctionWrapper<func> function;
}

#endif
