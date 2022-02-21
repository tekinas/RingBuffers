#ifndef FUNCTIONQUEUE_COMMON
#define FUNCTIONQUEUE_COMMON

#include "rb_detail.h"
#include <cstddef>
#include <functional>
#include <limits>
#include <locale>
#include <span>
#include <type_traits>

namespace rb {
    template<typename T>
    inline constexpr size_t memory_footprint = alignof(T) + sizeof(T) - 1;
}

namespace rb::detail {
    template<typename FunctionContext>
    inline constexpr size_t sentinel_region_size{memory_footprint<FunctionContext> +
                                                 FunctionContext::max_obj_footprint};

    template<typename FunctionContext>
    inline constexpr size_t min_buffer_size{2 * sentinel_region_size<FunctionContext>};

    //static_assert(sentinel_region_size <= std::numeric_limits<uint16_t>::max());

    template<typename Callable, typename FunctionContext, typename... CArgs>
    concept valid_callable = std::is_nothrow_constructible_v<Callable, CArgs...> &&
                             std::is_nothrow_destructible_v<Callable> &&
                             FunctionContext::template is_nothrow_invocable<Callable> &&
                             ((memory_footprint<Callable> + memory_footprint<FunctionContext>) <=
                              sentinel_region_size<FunctionContext>);

    template<typename Callable>
    concept empty_callable = std::is_empty_v<Callable> && std::is_nothrow_default_constructible_v<Callable>;

    template<typename FunctionContext>
    std::span<std::byte> allocate_buffer(allocator_type allocator, size_t buffer_size) noexcept {
        return {static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, alignof(FunctionContext))),
                buffer_size - sentinel_region_size<FunctionContext>};
    }

    template<typename FunctionContext>
    size_t buffer_size(std::span<std::byte> buffer) noexcept {
        return buffer.size() + sentinel_region_size<FunctionContext>;
    }

    template<typename FunctionContext>
    void deallocate_buffer(allocator_type allocator, std::span<std::byte> buffer) noexcept {
        allocator.deallocate_object(buffer.data(), buffer_size<FunctionContext>(buffer));
    }

    struct RingBuffer {
        std::byte *begin;
        std::byte *end;
        std::byte *input_pos;
        std::byte *output_pos;

        RingBuffer(std::span<std::byte> buffer, std::byte *input_pos, std::byte *output_pos) noexcept
            : begin{buffer.data()}, end{begin + buffer.size()}, input_pos{input_pos}, output_pos{output_pos} {}

        RingBuffer(std::span<std::byte> buffer, size_t input_offset, size_t output_offset) noexcept
            : RingBuffer{buffer, &buffer[input_offset], &buffer[output_offset]} {}

        RingBuffer(std::span<std::byte> buffer) noexcept : RingBuffer{buffer, buffer.data(), buffer.data()} {}

        operator std::span<std::byte>() const noexcept { return {begin, end}; }
    };

    struct Empty {
        Empty(auto &&...) noexcept {}
    };

    template<typename Callable, typename ReturnType, typename... Args>
    ReturnType invokeAndDestroy(void *data, Args... args) noexcept {
        if constexpr (empty_callable<Callable>) return std::invoke(Callable{}, static_cast<Args>(args)...);
        else {
            auto callable_ptr = static_cast<Callable *>(data);
            ScopeGaurd const destroy_functor = [&] { std::destroy_at(callable_ptr); };
            return std::invoke(*callable_ptr, static_cast<Args>(args)...);
        }
    }

    template<typename Callable>
    void destroy(void *data) noexcept {
        if constexpr (!empty_callable<Callable>) std::destroy_at(static_cast<Callable *>(data));
    }

    template<bool destroyNonInvoked, size_t mof, typename R, typename... Args>
    class FunctionContext {
    private:
        FunctionPtr<R(void *, Args...)> m_InvokeAndDestroy;
        uint16_t callable_offset;
        uint16_t stride;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, FunctionPtr<void(void *)>, Empty> m_Destroy;

    public:
        static constexpr auto max_obj_footprint = mof;

        template<typename Callable>
        static constexpr auto is_nothrow_invocable = std::is_nothrow_invocable_r_v<R, Callable, Args...>;

        template<typename Callable>
        FunctionContext(std::type_identity<Callable>, uint16_t callable_offset, uint16_t stride) noexcept
            : m_InvokeAndDestroy{invokeAndDestroy<Callable, R, Args...>},
              callable_offset{callable_offset}, stride{stride}, m_Destroy{destroy<Callable>} {}

        template<typename... CArgs>
        decltype(auto) operator()(CArgs &&...args) noexcept {
            return std::invoke(m_InvokeAndDestroy, getCallableAddr(), std::forward<CArgs>(args)...);
        }

        void destroyFO() noexcept { std::invoke(m_Destroy, getCallableAddr()); }

        uint16_t getStride() const noexcept { return stride; }

    private:
        void *getCallableAddr() const noexcept { return std::bit_cast<std::byte *>(this) + callable_offset; }
    };

    template<typename FunctionContext, typename Callable, typename... CArgs>
    std::byte *emplace(RingBuffer const &info, CArgs &&...args) noexcept {
        constexpr size_t callable_align = empty_callable<Callable> ? 1 : alignof(Callable);
        constexpr size_t callable_size = empty_callable<Callable> ? 0 : sizeof(Callable);

        auto const fc_ptr = info.input_pos;
        auto const callable_ptr = align<std::byte, callable_align>(fc_ptr + sizeof(FunctionContext));
        auto const next_addr = align<std::byte, alignof(FunctionContext)>(callable_ptr + callable_size);

        if ((next_addr < info.output_pos) ||
            ((info.input_pos >= info.output_pos) && ((next_addr < info.end) || (info.output_pos != info.begin)))) {

            auto const callable_offset = static_cast<uint16_t>(callable_ptr - fc_ptr);
            auto const stride = static_cast<uint16_t>(next_addr - fc_ptr);
            new (fc_ptr) FunctionContext{std::type_identity<Callable>{}, callable_offset, stride};
            if constexpr (!empty_callable<Callable>) new (callable_ptr) Callable{std::forward<CArgs>(args)...};

            return next_addr < info.end ? next_addr : info.begin;
        } else
            return nullptr;
    }

    template<typename FunctionContext>
    void destroyAllNonInvoked(RingBuffer const &info) noexcept {
        auto destroyAndGetStride = [](auto pos) {
            auto &functionCxt = *std::bit_cast<FunctionContext *>(pos);
            functionCxt.destroyFO();
            return functionCxt.getStride();
        };

        if (info.input_pos == info.output_pos) return;
        auto output_pos = info.output_pos;
        if (info.output_pos > info.input_pos) {
            while (output_pos < info.end) output_pos += destroyAndGetStride(output_pos);
            output_pos = info.begin;
        }

        while (output_pos != info.input_pos) output_pos += destroyAndGetStride(output_pos);
    }
}// namespace rb::detail

#endif
