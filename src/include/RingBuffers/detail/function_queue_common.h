#ifndef FUNCTIONQUEUE_COMMON
#define FUNCTIONQUEUE_COMMON

#include "rb_detail.h"
#include <functional>
#include <limits>
#include <locale>
#include <type_traits>

namespace rb {
    template<typename T>
    inline constexpr size_t memory_footprint = alignof(T) + sizeof(T) - 1;
}

namespace rb::detail {
    template<typename Callable>
    constexpr bool empty_callable = std::is_empty_v<Callable> &&std::is_nothrow_default_constructible_v<Callable>;

    struct RingBufferInfo {
        std::byte *input_pos;
        std::byte *output_pos;
        std::byte *buffer_start;
        std::byte *buffer_end;
    };

    template<bool condition>
    auto conditonal_value(auto tv, auto fv) noexcept {
        if constexpr (condition) return tv;
        else
            return fv;
    }

    class Empty {};

    template<bool destroyNonInvoked, typename R, typename... Args>
    struct FunctionContextData {
        FunctionPtr<R(void *, Args...)> m_InvokeAndDestroy;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, FunctionPtr<void(void *)>, Empty> m_Destroy;
        uint16_t callable_offset;
        uint16_t stride;
    };

    template<bool destroyNonInvoked, size_t max_obj_footprint, typename R, typename... Args>
    class FunctionContext {
    public:
        using FCData = FunctionContextData<destroyNonInvoked, R, Args...>;
        static constexpr size_t fcxt_footprint = memory_footprint<FCData>;
        static constexpr size_t sentinel_region_size = fcxt_footprint + max_obj_footprint;
        static constexpr size_t buffer_alignment = alignof(FCData);
        static constexpr size_t min_buffer_size = 2 * sentinel_region_size;

        static_assert(sentinel_region_size <= std::numeric_limits<uint16_t>::max());

        template<typename Callable, typename... CArgs>
        static constexpr bool is_valid_callable_v =
                std::is_nothrow_constructible_v<Callable, CArgs...> &&std::is_nothrow_destructible_v<Callable> &&
                        std::is_nothrow_invocable_r_v<R, Callable, Args...> &&
                ((memory_footprint<Callable> + fcxt_footprint) <= sentinel_region_size);

        template<typename... CArgs>
        decltype(auto) operator()(CArgs &&...args) const noexcept {
            return std::invoke(data.m_InvokeAndDestroy, getCallableAddr(), std::forward<CArgs>(args)...);
        }

        void destroyFO() const noexcept { std::invoke(data.m_Destroy, getCallableAddr()); }

        uint16_t getStride() const noexcept { return data.stride; }

        template<typename Callable, typename... CArgs>
        static std::byte *emplace(RingBufferInfo const &info, CArgs &&...args) noexcept {
            constexpr size_t callable_align = empty_callable<Callable> ? 1 : alignof(Callable);
            constexpr size_t callable_size = empty_callable<Callable> ? 0 : sizeof(Callable);

            auto const fc_ptr = info.input_pos;
            auto const callable_ptr = align<std::byte, callable_align>(fc_ptr + sizeof(FCData));
            auto const next_addr = align<std::byte, alignof(FCData)>(callable_ptr + callable_size);

            if ((next_addr < info.output_pos) ||
                ((info.input_pos >= info.output_pos) &&
                 ((next_addr < info.buffer_end) || (info.output_pos != info.buffer_start)))) {

                auto const callable_offset = static_cast<uint16_t>(callable_ptr - fc_ptr);
                auto const stride = static_cast<uint16_t>(next_addr - fc_ptr);
                new (fc_ptr) FunctionContext{FCData{invokeAndDestroy<Callable>,
                                                    conditonal_value<destroyNonInvoked>(destroy<Callable>, Empty{}),
                                                    callable_offset, stride}};
                if constexpr (!empty_callable<Callable>) new (callable_ptr) Callable{std::forward<CArgs>(args)...};

                return next_addr < info.buffer_end ? next_addr : info.buffer_start;
            } else
                return nullptr;
        }

        static void destroyAllNonInvoked(RingBufferInfo const &info) noexcept {
            auto destroyAndGetStride = [](auto pos) {
                auto const &functionCxt = *std::bit_cast<FunctionContext *>(pos);
                functionCxt.destroyFO();
                return functionCxt.getStride();
            };

            if (info.input_pos == info.output_pos) return;
            auto output_pos = info.output_pos;
            if (info.output_pos > info.input_pos) {
                while (output_pos < info.buffer_end) output_pos += destroyAndGetStride(output_pos);
                output_pos = info.buffer_start;
            }

            while (output_pos != info.input_pos) output_pos += destroyAndGetStride(output_pos);
        }

    private:
        FunctionContext(FCData data) noexcept : data{data} {}

        void *getCallableAddr() const noexcept { return std::bit_cast<std::byte *>(this) + data.callable_offset; }

        template<typename Callable>
        static R invokeAndDestroy(void *data, Args... args) noexcept {
            if constexpr (empty_callable<Callable>) return std::invoke(Callable{}, static_cast<Args>(args)...);
            else {
                auto callable_ptr = static_cast<Callable *>(data);
                ScopeGaurd const destroy_functor = [&] { std::destroy_at(callable_ptr); };
                return std::invoke(*callable_ptr, static_cast<Args>(args)...);
            }
        }

        template<typename Callable>
        static void destroy(void *data) noexcept {
            if constexpr (!empty_callable<Callable>) std::destroy_at(static_cast<Callable *>(data));
        }

        FCData data;
    };
}// namespace rb::detail

#endif
