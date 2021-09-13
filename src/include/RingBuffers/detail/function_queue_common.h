#ifndef FUNCTIONQUEUE_COMMON
#define FUNCTIONQUEUE_COMMON

#include "rb_detail.h"
#include <functional>
#include <limits>

namespace rb {
    template<typename T>
    constexpr size_t memory_footprint = alignof(T) + sizeof(T) - 1;
}

namespace rb::detail {
    template<bool destroyNonInvoked, typename R, typename... Args>
    class FunctionContext {
    public:
        R operator()(Args... args) const noexcept {
            return m_InvokeAndDestroy(getCallableAddr(), static_cast<Args>(args)...);
        }

        void destroyFO() const noexcept { m_Destroy(getCallableAddr()); }

        uint16_t getStride() const noexcept { return stride; }

        template<typename Callable>
        static void construct(void *addr, uint16_t callable_offset, uint16_t stride) noexcept {
            new (addr) FunctionContext{invokeAndDestroy<Callable>, destroy<Callable>, callable_offset, stride};
        }

    private:
        FunctionContext(auto a1, auto a2, auto a3, auto a4) noexcept
            : m_InvokeAndDestroy{a1}, m_Destroy{a2}, callable_offset{a3}, stride{a4} {}

        void *getCallableAddr() const noexcept { return std::bit_cast<std::byte *>(this) + callable_offset; }

        template<typename Callable>
        static R invokeAndDestroy(void *data, Args... args) noexcept {
            auto &callable = *static_cast<Callable *>(data);
            ScopeGaurd const destroy_functor{[&] { std::destroy_at(&callable); }};

            return std::invoke(callable, static_cast<Args>(args)...);
        }

        template<typename Callable>
        static void destroy(void *data) noexcept {
            std::destroy_at(static_cast<Callable *>(data));
        }

        FunctionPtr<R(void *, Args...)> m_InvokeAndDestroy;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, FunctionPtr<void(void *)>, Empty> m_Destroy;
        uint16_t callable_offset;
        uint16_t stride;
    };

    struct RingBufferInfo {
        std::byte *input_pos;
        std::byte *output_pos;
        std::byte *buffer_start;
        std::byte *buffer_end;
    };

    template<typename FunctionContextType, typename Callable, typename... CArgs>
    std::byte *emplace(RingBufferInfo const &info, CArgs &&...args) noexcept {
        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const fc_ptr = info.input_pos;
        auto const callable_ptr = align<std::byte, callable_align>(fc_ptr + sizeof(FunctionContextType));
        auto const next_addr = align<std::byte, alignof(FunctionContextType)>(callable_ptr + callable_size);

        if ((next_addr < info.output_pos) ||
            ((info.input_pos >= info.output_pos) &&
             ((next_addr < info.buffer_end) || (info.output_pos != info.buffer_start)))) {

            auto const callable_offset = static_cast<uint16_t>(callable_ptr - fc_ptr);
            auto const stride = static_cast<uint16_t>(next_addr - fc_ptr);
            FunctionContextType::template construct<Callable>(fc_ptr, callable_offset, stride);
            new (callable_ptr) Callable{std::forward<CArgs>(args)...};

            return next_addr < info.buffer_end ? next_addr : info.buffer_start;
        } else
            return nullptr;
    }


    template<typename FunctionContextType>
    void destroyAllNonInvoked(RingBufferInfo const &info) {
        auto destroyAndGetStride = [](auto pos) {
            auto const &functionCxt = *std::bit_cast<FunctionContextType *>(pos);
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
}// namespace rb::detail

#endif
