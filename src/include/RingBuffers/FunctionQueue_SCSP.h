#ifndef FUNCTIONQUEUE_SCSP
#define FUNCTIONQUEUE_SCSP

#include "detail/function_queue_common.h"
#include "detail/rb_detail.h"
#include <atomic>
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
    class FunctionQueue_SCSP {};

    template<typename R, typename... Args, bool destroyNonInvoked, size_t max_obj_footprint>
    class FunctionQueue_SCSP<R(Args...), destroyNonInvoked, max_obj_footprint> {
    private:
        using FunctionContextType = detail::FunctionContext<destroyNonInvoked, max_obj_footprint, R, Args...>;

    public:
        using allocator_type = std::pmr::polymorphic_allocator<>;

        static constexpr size_t min_buffer_size() noexcept { return FunctionContextType::min_buffer_size; }

        FunctionQueue_SCSP(size_t buffer_size, allocator_type allocator = {}) noexcept
            : m_Buffer{static_cast<std::byte *>(
                      allocator.allocate_bytes(buffer_size, FunctionContextType::buffer_alignment))},
              m_BufferEnd{m_Buffer + buffer_size - FunctionContextType::sentinel_region_size}, m_InputPos{m_Buffer},
              m_OutputPos{m_Buffer}, m_Allocator{allocator} {}

        ~FunctionQueue_SCSP() {
            if constexpr (destroyNonInvoked) FunctionContextType::destroyAllNonInvoked(getBufferInfo());
            m_Allocator.deallocate_bytes(m_Buffer, buffer_size(), FunctionContextType::buffer_alignment);
        }

        size_t buffer_size() const noexcept {
            return m_BufferEnd - m_Buffer + FunctionContextType::sentinel_region_size;
        }

        bool empty() const noexcept {
            return m_InputPos.load(std::memory_order::acquire) == m_OutputPos.load(std::memory_order::relaxed);
        }

        template<typename... CArgs>
        decltype(auto) call_and_pop(CArgs &&...args) noexcept {
            auto const output_pos = m_OutputPos.load(std::memory_order::relaxed);
            auto const &functionCxt = *std::bit_cast<FunctionContextType *>(output_pos);

            detail::ScopeGaurd const set_next_output_pos{[&, next_addr = output_pos + functionCxt.getStride()] {
                auto const next_pos = next_addr < m_BufferEnd ? next_addr : m_Buffer;
                m_OutputPos.store(next_pos, std::memory_order::release);
            }};

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
                m_InputPos.store(next_addr, std::memory_order::release);
                return true;
            } else
                return false;
        }

    private:
        auto getBufferInfo() const noexcept {
            return detail::RingBufferInfo{.input_pos = m_InputPos.load(std::memory_order::relaxed),
                                          .output_pos = m_OutputPos.load(std::memory_order::acquire),
                                          .buffer_start = m_Buffer,
                                          .buffer_end = m_BufferEnd};
        }

        std::byte *const m_Buffer;
        std::byte *const m_BufferEnd;

        std::atomic<std::byte *> m_InputPos;
        mutable std::atomic<std::byte *> m_OutputPos;

        allocator_type m_Allocator;
    };
}// namespace rb
#endif
