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
        using FunctionContextType = detail::FunctionContext<destroyNonInvoked, max_obj_footprint, R, Args...>;

    public:
        using allocator_type = std::pmr::polymorphic_allocator<>;

        static constexpr size_t min_buffer_size() noexcept { return FunctionContextType::min_buffer_size; }

        FunctionQueue(size_t buffer_size, allocator_type allocator = {}) noexcept
            : m_Buffer{static_cast<std::byte *>(
                      allocator.allocate_bytes(buffer_size, FunctionContextType::buffer_alignment))},
              m_BufferEnd{m_Buffer + buffer_size - FunctionContextType::sentinel_region_size}, m_InputPos{m_Buffer},
              m_OutputPos{m_Buffer}, m_Allocator{allocator} {}

        ~FunctionQueue() noexcept {
            if constexpr (destroyNonInvoked) destroyAllNonInvoked(getBufferInfo());
            m_Allocator.deallocate_bytes(m_Buffer, buffer_size(), FunctionContextType::buffer_alignment);
        }

        size_t buffer_size() const noexcept {
            return m_BufferEnd - m_Buffer + FunctionContextType::sentinel_region_size;
        }

        void clear() noexcept {
            if constexpr (destroyNonInvoked) destroyAllNonInvoked(getBufferInfo());

            m_InputPos = m_Buffer;
            m_OutputPos = m_Buffer;
        }

        bool empty() const noexcept { return m_InputPos == m_OutputPos; }

        template<typename... CArgs>
        decltype(auto) call_and_pop(CArgs &&...args) noexcept {
            auto const &functionCxt = *std::bit_cast<FunctionContextType *>(m_OutputPos);

            detail::ScopeGaurd const set_next_output_pos = [&, next_addr = m_OutputPos + functionCxt.getStride()] {
                m_OutputPos = next_addr < m_BufferEnd ? next_addr : m_Buffer;
            };

            return std::invoke(functionCxt, std::forward<CArgs>(args)...);
        }

        template<typename T>
        bool push(T &&callable) noexcept {
            using Callable = std::decay_t<T>;
            return emplace<Callable>(std::forward<T>(callable));
        }

        template<typename Callable, typename... CArgs>
        requires FunctionContextType::template is_valid_callable_v<Callable, CArgs...> bool
        emplace(CArgs &&...args) noexcept {
            if (auto const next_addr = FunctionContextType::template emplace<Callable>(getBufferInfo(),
                                                                                       std::forward<CArgs>(args)...)) {
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

        std::byte *const m_Buffer;
        std::byte *const m_BufferEnd;

        std::byte *m_InputPos;
        mutable std::byte *m_OutputPos;

        allocator_type m_Allocator;
    };
}// namespace rb

#endif
