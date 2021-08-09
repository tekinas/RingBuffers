#ifndef FUNCTIONQUEUE
#define FUNCTIONQUEUE

#include "rb_detail.h"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

template<typename T, bool destroyNonInvoked = true, size_t max_obj_footprint = alignof(std::max_align_t) + 512>
class FunctionQueue {};

template<typename R, typename... Args, bool destroyNonInvoked, size_t max_obj_footprint>
class FunctionQueue<R(Args...), destroyNonInvoked, max_obj_footprint> {
private:
    class Storage;

    class FunctionContext {
    public:
        using InvokeAndDestroy = R (*)(void *callable_ptr, Args...) noexcept;
        using Destroy = void (*)(void *callable_ptr) noexcept;

        struct ReadData {
            InvokeAndDestroy function;
            void *callable_ptr;
            std::byte *next_addr;
        };

        ReadData getReadData() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const function = std::bit_cast<InvokeAndDestroy>(getFp(fp_offset));
            return {function, this_ + callable_offset, this_ + stride};
        }

        std::byte *destroyFO() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const destroy_functor = std::bit_cast<Destroy>(getFp(destroyFp_offset));

            destroy_functor(this_ + callable_offset);
            return this_ + stride;
        }

    private:
        template<typename Callable>
        FunctionContext(rb_detail::Type<Callable>, uint16_t callable_offset, uint16_t stride) noexcept
            : fp_offset{getFpOffset(&invokeAndDestroy<Callable>)}, destroyFp_offset{getFpOffset(&destroy<Callable>)},
              callable_offset{callable_offset}, stride{stride} {}

        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, rb_detail::Empty> destroyFp_offset;
        uint16_t const callable_offset;
        uint16_t const stride;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return fc_ptr; }

        std::byte *getNextAddr() const noexcept { return next_addr; }

        Storage getWrapped(std::byte *wrap_addr) const noexcept { return {fc_ptr, callable_ptr, wrap_addr}; }

        template<typename Callable, typename... CArgs>
        void construct(rb_detail::Type<Callable>, CArgs &&...args) const noexcept {
            uint16_t const callable_offset = callable_ptr - fc_ptr;
            uint16_t const stride = next_addr - fc_ptr;

            new (fc_ptr) FunctionContext{rb_detail::Type<Callable>{}, callable_offset, stride};
            new (callable_ptr) Callable{std::forward<CArgs>(args)...};
        }

        template<size_t obj_align, size_t obj_size>
        static Storage getAlignedStorage(std::byte *buffer_start) noexcept {
            auto const fc_ptr = buffer_start;
            auto const obj_ptr = align<std::byte, obj_align>(fc_ptr + sizeof(FunctionContext));
            auto const next_addr = align<std::byte, alignof(FunctionContext)>(obj_ptr + obj_size);
            return {fc_ptr, obj_ptr, next_addr};
        }

    private:
        Storage(std::byte *fc_ptr, std::byte *callable_ptr, std::byte *next_addr) noexcept
            : fc_ptr{fc_ptr}, callable_ptr{callable_ptr}, next_addr{next_addr} {}

        std::byte *fc_ptr{};
        std::byte *callable_ptr;
        std::byte *next_addr;
    };

    static constexpr inline size_t function_context_footprint = alignof(FunctionContext) + sizeof(FunctionContext);
    static constexpr inline size_t sentinel_region_size = function_context_footprint + max_obj_footprint;

public:
    static constexpr inline size_t BUFFER_ALIGNMENT = alignof(FunctionContext);

    static_assert(sentinel_region_size <= std::numeric_limits<uint16_t>::max());

    template<typename Callable, typename... CArgs>
    static constexpr bool is_valid_callable_v =
            std::is_nothrow_constructible_v<Callable, CArgs...> &&std::is_nothrow_destructible_v<Callable> &&
                    std::is_nothrow_invocable_r_v<R, Callable, Args...> &&
            ((alignof(Callable) + sizeof(Callable) + function_context_footprint) <= sentinel_region_size);

    FunctionQueue(std::byte *buffer, size_t size) noexcept
        : m_InputPos{buffer}, m_OutputPos{buffer}, m_Buffer{buffer}, m_BufferEnd{buffer + size - sentinel_region_size} {
    }

    std::byte *buffer() const noexcept { return m_Buffer; }

    size_t buffer_size() const noexcept { return m_BufferEnd - m_Buffer; }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputPos = m_Buffer;
        m_OutputPos = m_Buffer;
    }

    ~FunctionQueue() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();
    }

    bool empty() const noexcept { return m_InputPos == m_OutputPos; }

    R call_and_pop(Args... args) const noexcept {
        auto const functionCxtPtr = std::bit_cast<FunctionContext *>(m_OutputPos);
        auto const read_data = functionCxtPtr->getReadData();

        rb_detail::ScopeGaurd const set_next_output_pos{
                [&] { m_OutputPos = read_data.next_addr < m_BufferEnd ? read_data.next_addr : m_Buffer; }};

        return read_data.function(read_data.callable_ptr, std::forward<Args>(args)...);
    }

    template<auto invokable>
    requires std::is_nothrow_invocable_r_v<R, decltype(invokable), Args...>
    bool push_back() noexcept {
        return push_back([](Args... args) noexcept {
            if constexpr (std::is_function_v<std::remove_pointer_t<decltype(invokable)>>)
                return invokable(std::forward<Args>(args)...);
            else
                return std::invoke(invokable, std::forward<Args>(args)...);
        });
    }

    template<typename T>
    bool push_back(T &&callable) noexcept {
        using Callable = std::decay_t<T>;
        return emplace_back<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    requires is_valid_callable_v<Callable, CArgs...>
    bool emplace_back(CArgs &&...args) noexcept {
        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const storage = getStorage<callable_align, callable_size>();
        if (!storage) return false;

        storage.construct(rb_detail::Type<Callable>{}, std::forward<CArgs>(args)...);
        m_InputPos = storage.getNextAddr();

        return true;
    }

private:
    template<typename T, size_t alignment = alignof(T)>
    static constexpr T *align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + alignment) & -alignment);
    }

    template<typename Callable>
    static R invokeAndDestroy(void *data, Args... args) noexcept {
        auto const functor_ptr = static_cast<Callable *>(data);
        rb_detail::ScopeGaurd const destroy_functor{[&] { std::destroy_at(functor_ptr); }};

        return std::invoke(*functor_ptr, std::forward<Args>(args)...);
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
    }

    void destroyAllFO() {
        auto destroyAndGetNextPos = [](auto pos) {
            auto const functionCxtPtr = std::bit_cast<FunctionContext *>(pos);
            return functionCxtPtr->destroyFO();
        };

        auto const input_pos = m_InputPos;
        auto output_pos = m_OutputPos;

        if (input_pos == output_pos) return;
        else if (output_pos > input_pos) {
            while (output_pos < m_BufferEnd) { output_pos = destroyAndGetNextPos(output_pos); }
            output_pos = m_Buffer;
        }

        while (output_pos != input_pos) { output_pos = destroyAndGetNextPos(output_pos); }
    }

    template<size_t obj_align, size_t obj_size>
    Storage getStorage() const noexcept {
        auto const input_pos = m_InputPos;
        auto const output_pos = m_OutputPos;
        auto const buffer_start = m_Buffer;
        auto const buffer_end = m_BufferEnd;

        constexpr auto getAlignedStorage = Storage::template getAlignedStorage<obj_align, obj_size>;
        if (auto const storage = getAlignedStorage(input_pos); input_pos >= output_pos) {
            if (storage.getNextAddr() < buffer_end) [[likely]]
                return storage;
            else if (output_pos != buffer_start) {
                return storage.getWrapped(buffer_start);
            }
        } else if (storage.getNextAddr() < output_pos)
            return storage;

        return {};
    }

    static void fp_base_func() noexcept {}

    static void *getFp(uint32_t fp_offset) noexcept {
        const uintptr_t fp_base =
                std::bit_cast<uintptr_t>(&fp_base_func) & (static_cast<uintptr_t>(0XFFFFFFFF00000000lu));
        return std::bit_cast<void *>(fp_base + fp_offset);
    }

    template<typename FR, typename... FArgs>
    static uint32_t getFpOffset(FR (*fp)(FArgs...)) noexcept {
        return static_cast<uint32_t>(std::bit_cast<uintptr_t>(fp));
    }

private:
    std::byte *m_InputPos;
    mutable std::byte *m_OutputPos;

    std::byte *const m_Buffer;
    std::byte *const m_BufferEnd;
};

#endif
