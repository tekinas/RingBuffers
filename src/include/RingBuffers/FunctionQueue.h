#ifndef FUNCTIONQUEUE
#define FUNCTIONQUEUE

#include "detail/function_queue_common.h"
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

namespace rb {
    template<typename T, bool destroyNonInvoked = true,
             size_t max_obj_footprint = memory_footprint<std::aligned_storage_t<128>>>
    class FunctionQueue {};

    template<typename R, typename... Args, bool destroyNonInvoked, size_t max_obj_footprint>
    class FunctionQueue<R(Args...), destroyNonInvoked, max_obj_footprint> {
    private:
        using FunctionContextType = detail::FunctionContext<destroyNonInvoked, R, Args...>;

        static constexpr size_t fcxt_footprint = memory_footprint<FunctionContextType>;
        static constexpr size_t sentinel_region_size = fcxt_footprint + max_obj_footprint;
        static constexpr size_t buffer_alignment = alignof(FunctionContextType);
        static_assert(sentinel_region_size <= std::numeric_limits<uint16_t>::max());

        template<typename Callable, typename... CArgs>
        static constexpr bool is_valid_callable_v =
                std::is_nothrow_constructible_v<Callable, CArgs...> &&std::is_nothrow_destructible_v<Callable> &&
                        std::is_nothrow_invocable_r_v<R, Callable, Args...> &&
                ((memory_footprint<Callable> + fcxt_footprint) <= sentinel_region_size);

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
            auto const &functionCxt = *std::bit_cast<FunctionContextType *>(m_OutputPos);

            detail::ScopeGaurd const set_next_output_pos{[&, next_addr = m_OutputPos + functionCxt.getStride()] {
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
            if (auto const next_addr =
                        detail::emplace<FunctionContextType, Callable>(getBufferInfo(), std::forward<CArgs>(args)...)) {
                m_InputPos = next_addr;
                return true;
            } else
                return false;
        }

    private:
        auto getBufferInfo() const noexcept {
            return detail::RingBufferInfo{.input_pos = m_InputPos,
                                          .output_pos = m_OutputPos,
                                          .buffer_start = m_Buffer,
                                          .buffer_end = m_BufferEnd};
        }

        void destroyAllNonInvoked() noexcept { detail::destroyAllNonInvoked<FunctionContextType>(getBufferInfo()); }

        std::byte *const m_Buffer;
        std::byte *const m_BufferEnd;

        std::byte *m_InputPos;
        mutable std::byte *m_OutputPos;
        allocator_type m_Allocator;
    };
}// namespace rb

#endif
