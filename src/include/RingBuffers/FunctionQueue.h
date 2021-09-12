#ifndef FUNCTIONQUEUE
#define FUNCTIONQUEUE

#include "detail/rb_detail.h"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

template<typename T, bool destroyNonInvoked = true, size_t max_obj_footprint = alignof(std::max_align_t) + 128>
class FunctionQueue {};

template<typename R, typename... Args, bool destroyNonInvoked, size_t max_obj_footprint>
class FunctionQueue<R(Args...), destroyNonInvoked, max_obj_footprint> {
private:
    class FunctionContext {
    public:
        R operator()(Args... args) const noexcept {
            return m_InvokeAndDestroy(getCallableAddr(), static_cast<Args>(args)...);
        }

        void destroyFO() const noexcept { m_Destroy(getCallableAddr()); }

        uint16_t getStride() const noexcept { return stride; }

        template<typename Callable>
        FunctionContext(rb::detail::type_tag<Callable>, uint16_t callable_offset, uint16_t stride) noexcept
            : m_InvokeAndDestroy{invokeAndDestroy<Callable>}, m_Destroy{destroy<Callable>},
              callable_offset{callable_offset}, stride{stride} {}

    private:
        void *getCallableAddr() const noexcept { return std::bit_cast<std::byte *>(this) + callable_offset; }

        rb::detail::FunctionPtr<R(void *, Args...)> m_InvokeAndDestroy;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, rb::detail::FunctionPtr<void(void *)>,
                                                 rb::detail::Empty>
                m_Destroy;
        uint16_t const callable_offset;
        uint16_t const stride;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return fc_ptr; }

        std::byte *getNextAddr() const noexcept { return next_addr; }

        template<typename Callable, typename... CArgs>
        void construct(rb::detail::type_tag<Callable>, CArgs &&...args) const noexcept {
            auto const callable_offset = static_cast<uint16_t>(callable_ptr - fc_ptr);
            auto const stride = static_cast<uint16_t>(next_addr - fc_ptr);

            new (fc_ptr) FunctionContext{rb::detail::type<Callable>, callable_offset, stride};
            new (callable_ptr) Callable{std::forward<CArgs>(args)...};
        }

        template<size_t obj_align, size_t obj_size>
        static Storage getAlignedStorage(std::byte *buffer_start) noexcept {
            auto const fc_ptr = buffer_start;
            auto const obj_ptr = rb::detail::align<std::byte, obj_align>(fc_ptr + sizeof(FunctionContext));
            auto const next_addr = rb::detail::align<std::byte, alignof(FunctionContext)>(obj_ptr + obj_size);
            return {fc_ptr, obj_ptr, next_addr};
        }

    private:
        Storage(std::byte *fc_ptr, std::byte *callable_ptr, std::byte *next_addr) noexcept
            : fc_ptr{fc_ptr}, callable_ptr{callable_ptr}, next_addr{next_addr} {}

        std::byte *fc_ptr{};
        std::byte *callable_ptr;
        std::byte *next_addr;
    };

    static constexpr size_t function_context_footprint = alignof(FunctionContext) + sizeof(FunctionContext);
    static constexpr size_t sentinel_region_size = function_context_footprint + max_obj_footprint;
    static constexpr size_t buffer_alignment = alignof(FunctionContext);
    static_assert(sentinel_region_size <= std::numeric_limits<uint16_t>::max());

    template<typename Callable, typename... CArgs>
    static constexpr bool is_valid_callable_v =
            std::is_nothrow_constructible_v<Callable, CArgs...> &&std::is_nothrow_destructible_v<Callable> &&
                    std::is_nothrow_invocable_r_v<R, Callable, Args...> &&
            ((alignof(Callable) + sizeof(Callable) + function_context_footprint) <= sentinel_region_size);

public:
    using allocator_type = std::pmr::polymorphic_allocator<>;

    FunctionQueue(size_t buffer_size, allocator_type allocator = {}) noexcept
        : m_Buffer{static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, buffer_alignment))},
          m_BufferEnd{m_Buffer + buffer_size - sentinel_region_size}, m_InputPos{m_Buffer}, m_OutputPos{m_Buffer},
          m_Allocator{allocator} {}

    ~FunctionQueue() noexcept {
        if constexpr (destroyNonInvoked) destroyAllNonInvoked();
        m_Allocator.deallocate_bytes(m_Buffer, buffer_size(), buffer_alignment);
    }

    size_t buffer_size() const noexcept { return m_BufferEnd - m_Buffer + sentinel_region_size; }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllNonInvoked();

        m_InputPos = m_Buffer;
        m_OutputPos = m_Buffer;
    }

    bool empty() const noexcept { return m_InputPos == m_OutputPos; }

    R call_and_pop(Args... args) const noexcept {
        auto const &functionCxt = *std::bit_cast<FunctionContext *>(m_OutputPos);

        rb::detail::ScopeGaurd const set_next_output_pos{[&, next_addr = m_OutputPos + functionCxt.getStride()] {
            m_OutputPos = next_addr < m_BufferEnd ? next_addr : m_Buffer;
        }};

        return functionCxt(static_cast<Args>(args)...);
    }

    template<typename T>
    bool push(T &&callable) noexcept {
        using Callable = std::decay_t<T>;
        return emplace<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    requires is_valid_callable_v<Callable, CArgs...>
    bool emplace(CArgs &&...args) noexcept {
        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const storage = getStorage<callable_align, callable_size>();
        if (!storage) return false;

        storage.construct(rb::detail::type<Callable>, std::forward<CArgs>(args)...);
        auto next_addr = storage.getNextAddr();
        m_InputPos = next_addr < m_BufferEnd ? next_addr : m_Buffer;

        return true;
    }

private:
    template<typename Callable>
    static R invokeAndDestroy(void *data, Args... args) noexcept {
        auto &callable = *static_cast<Callable *>(data);
        rb::detail::ScopeGaurd const destroy_functor{[&] { std::destroy_at(&callable); }};

        return std::invoke(callable, static_cast<Args>(args)...);
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
    }

    void destroyAllNonInvoked() {
        auto destroyAndGetStride = [](auto pos) {
            auto const &functionCxt = *std::bit_cast<FunctionContext *>(pos);
            functionCxt.destroyFO();
            return functionCxt.getStride();
        };

        if (m_InputPos == m_OutputPos) return;

        auto output_pos = m_OutputPos;
        if (output_pos > m_InputPos) {
            while (output_pos < m_BufferEnd) output_pos += destroyAndGetStride(output_pos);
            output_pos = m_Buffer;
        }

        while (output_pos != m_InputPos) output_pos += destroyAndGetStride(output_pos);
    }

    template<size_t obj_align, size_t obj_size>
    Storage getStorage() const noexcept {
        if (auto const storage = Storage::template getAlignedStorage<obj_align, obj_size>(m_InputPos);
            (storage.getNextAddr() < m_OutputPos) ||
            ((m_InputPos >= m_OutputPos) && ((storage.getNextAddr() < m_BufferEnd) || (m_OutputPos != m_Buffer))))
            return storage;
        else
            return {};
    }

private:
    std::byte *const m_Buffer;
    std::byte *const m_BufferEnd;

    std::byte *m_InputPos;
    mutable std::byte *m_OutputPos;
    allocator_type m_Allocator;
};

#endif
