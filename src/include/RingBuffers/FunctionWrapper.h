#ifndef FUNCTION_WRAPPER
#define FUNCTION_WRAPPER

#include <concepts>
#include <type_traits>
#include <utility>

namespace rb {
    namespace detail {
        template<typename T>
        concept function_ptr = std::is_function_v<std::remove_pointer_t<T>>;
    }

    template<detail::function_ptr auto func>
    constexpr inline auto function = []<typename... Args>(Args &&...args) noexcept(
                                             std::is_nothrow_invocable_v<decltype(func), Args...>) -> decltype(auto) {
        return func(std::forward<Args>(args)...);
    };
}// namespace rb

#endif
