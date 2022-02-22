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
        using FunctionContext = detail::FunctionContext<destroyNonInvoked, max_obj_footprint, R, Args...>;

    public:
        static constexpr auto min_buffer_size = detail::min_buffer_size<FunctionContext>;

        explicit FunctionQueue(size_t buffer_size, allocator_type allocator = {}) noexcept
            : m_Buffer{detail::allocate_buffer<FunctionContext>(allocator, buffer_size)}, m_Allocator{allocator} {}

        ~FunctionQueue() {
            if constexpr (destroyNonInvoked) destroyAllNonInvoked<FunctionContext>(m_Buffer);
            detail::deallocate_buffer<FunctionContext>(m_Allocator, m_Buffer);
        }

        size_t buffer_size() const noexcept { return detail::buffer_size<FunctionContext>(m_Buffer); }

        void clear() noexcept {
            if constexpr (destroyNonInvoked) destroyAllNonInvoked<FunctionContext>(m_Buffer);
            m_Buffer.input_pos = m_Buffer.begin;
            m_Buffer.output_pos = m_Buffer.begin;
        }

        bool empty() const noexcept { return m_Buffer.input_pos == m_Buffer.output_pos; }

        template<typename... CArgs>
        decltype(auto) call_and_pop(CArgs &&...args) noexcept {
            auto &functionCxt = *std::bit_cast<FunctionContext *>(m_Buffer.output_pos);
            detail::ScopeGaurd const set_next_output_pos = [&,
                                                            next_addr = m_Buffer.output_pos + functionCxt.getStride()] {
                m_Buffer.output_pos = next_addr < m_Buffer.end ? next_addr : m_Buffer.begin;
            };
            return std::invoke(functionCxt, std::forward<CArgs>(args)...);
        }

        template<typename T>
        bool push(T &&callable) noexcept {
            using Callable = std::remove_cvref_t<T>;
            return emplace<Callable>(std::forward<T>(callable));
        }

        template<typename Callable, typename... CArgs>
        requires detail::valid_callable<Callable, FunctionContext, CArgs...>
        bool emplace(CArgs &&...args) noexcept {
            if (auto const next_addr =
                        detail::emplace<FunctionContext, Callable>(m_Buffer, std::forward<CArgs>(args)...)) {
                m_Buffer.input_pos = next_addr;
                return true;
            } else
                return false;
        }

    private:
        detail::RingBuffer m_Buffer;
        allocator_type m_Allocator;
    };
}// namespace rb

#endif
