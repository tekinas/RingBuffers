#ifndef FUNCTIONQUEUE_SCSP
#define FUNCTIONQUEUE_SCSP

#include "rb_detail.h"
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

template<typename T, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked = true,
         size_t max_obj_footprint = alignof(std::max_align_t) + 128>
class FunctionQueue_SCSP {};

template<typename R, typename... Args, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked,
         size_t max_obj_footprint>
class FunctionQueue_SCSP<R(Args...), isReadProtected, isWriteProtected, destroyNonInvoked, max_obj_footprint> {
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
            auto const function = getFp<InvokeAndDestroy>(fp_offset);
            return {function, this_ + callable_offset, this_ + stride};
        }

        std::byte *destroyFO() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const destroy_functor = getFp<Destroy>(destroyFp_offset);

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

    FunctionQueue_SCSP(std::byte *buffer, size_t size) noexcept
        : m_InputPos{buffer}, m_OutputPos{buffer}, m_Buffer{buffer}, m_BufferEnd{buffer + size - sentinel_region_size} {
    }

    ~FunctionQueue_SCSP() noexcept {
        if constexpr (destroyNonInvoked) {
            if constexpr (isReadProtected)
                while (m_ReadFlag.test_and_set(std::memory_order::acquire))
                    ;
            if constexpr (isWriteProtected)
                while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                    ;

            destroyAllNonInvoked();
        }
    }

    std::byte *buffer() const noexcept { return m_Buffer; }

    size_t buffer_size() const noexcept { return m_BufferEnd - m_Buffer + sentinel_region_size; }

    bool empty() const noexcept {
        return m_InputPos.load(std::memory_order::acquire) == m_OutputPos.load(std::memory_order::relaxed);
    }

    void clear() noexcept {
        if constexpr (isReadProtected)
            while (m_ReadFlag.test_and_set(std::memory_order::acquire))
                ;
        if constexpr (isWriteProtected)
            while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                ;

        rb_detail::ScopeGaurd release_read_n_write_lock{[&] {
            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        if constexpr (destroyNonInvoked) destroyAllNonInvoked();

        m_InputPos.store(m_Buffer, std::memory_order::relaxed);
        m_OutputPos.store(m_Buffer, std::memory_order::relaxed);
    }

    bool reserve() const noexcept {
        if constexpr (isReadProtected) {
            if (m_ReadFlag.test_and_set(std::memory_order::acquire)) return false;
            if (empty()) {
                m_ReadFlag.clear(std::memory_order::relaxed);
                return false;
            } else
                return true;
        } else
            return !empty();
    }

    R call_and_pop(Args... args) const noexcept {
        auto const output_pos = m_OutputPos.load(std::memory_order::relaxed);
        auto const functionCxtPtr = std::bit_cast<FunctionContext *>(output_pos);
        auto const read_data = functionCxtPtr->getReadData();

        rb_detail::ScopeGaurd const update_state{[&] {
            auto const next_addr = read_data.next_addr < m_BufferEnd ? read_data.next_addr : m_Buffer;
            m_OutputPos.store(next_addr, std::memory_order::release);
            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);
        }};

        return read_data.function(read_data.callable_ptr, std::forward<Args>(args)...);
    }

    template<auto invokable>
    requires std::is_nothrow_invocable_r_v<R, decltype(invokable), Args...>
    bool push() noexcept {
        return push([](Args... args) noexcept {
            if constexpr (std::is_function_v<std::remove_pointer_t<decltype(invokable)>>)
                return invokable(std::forward<Args>(args)...);
            else
                return std::invoke(invokable, std::forward<Args>(args)...);
        });
    }

    template<typename T>
    bool push(T &&callable) noexcept {
        using Callable = std::decay_t<T>;
        return emplace<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    requires is_valid_callable_v<Callable, CArgs...>
    bool emplace(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const storage = getStorage<callable_align, callable_size>();
        if (!storage) return false;

        storage.construct(rb_detail::Type<Callable>{}, std::forward<CArgs>(args)...);
        auto const next_addr = storage.getNextAddr() < m_BufferEnd ? storage.getNextAddr() : m_Buffer;
        m_InputPos.store(next_addr, std::memory_order::release);

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

    void destroyAllNonInvoked() {
        auto destroyAndGetNextPos = [](auto pos) {
            auto const functionCxtPtr = std::bit_cast<FunctionContext *>(pos);
            return functionCxtPtr->destroyFO();
        };

        auto const input_pos = m_InputPos.load(std::memory_order::acquire);
        auto output_pos = m_OutputPos.load(std::memory_order::acquire);

        if (input_pos == output_pos) return;
        else if (output_pos > input_pos) {
            while (output_pos < m_BufferEnd) output_pos = destroyAndGetNextPos(output_pos);
            output_pos = m_Buffer;
        }

        while (output_pos != input_pos) output_pos = destroyAndGetNextPos(output_pos);
    }

    template<size_t obj_align, size_t obj_size>
    Storage getStorage() const noexcept {
        auto const input_pos = m_InputPos.load(std::memory_order::relaxed);
        auto const output_pos = m_OutputPos.load(std::memory_order::acquire);
        auto const buffer_start = m_Buffer;
        auto const buffer_end = m_BufferEnd;

        constexpr auto getAlignedStorage = Storage::template getAlignedStorage<obj_align, obj_size>;
        if (auto const storage = getAlignedStorage(input_pos);
            (storage.getNextAddr() < output_pos) ||
            ((input_pos >= output_pos) && ((storage.getNextAddr() < buffer_end) || (output_pos - buffer_start))))
            return storage;

        return {};
    }

    static void fp_base_func() noexcept {}

    template<typename FPtr>
    requires std::is_function_v<std::remove_pointer_t<FPtr>>
    static auto getFp(uint32_t fp_offset) noexcept {
        const uintptr_t fp_base =
                std::bit_cast<uintptr_t>(&fp_base_func) & static_cast<uintptr_t>(0XFFFFFFFF00000000lu);
        return std::bit_cast<FPtr>(fp_base + fp_offset);
    }

    template<typename FPtr>
    requires std::is_function_v<std::remove_pointer_t<FPtr>>
    static uint32_t getFpOffset(FPtr fp) noexcept { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(fp)); }

private:
    std::atomic<std::byte *> m_InputPos;
    mutable std::atomic<std::byte *> m_OutputPos;

    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, rb_detail::Empty> m_ReadFlag{};
    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, rb_detail::Empty> m_WriteFlag{};

    std::byte *const m_Buffer;
    std::byte *const m_BufferEnd;
};

#endif