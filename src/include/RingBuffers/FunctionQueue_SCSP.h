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
        using FunctionContext = detail::FunctionContext<destroyNonInvoked, max_obj_footprint, R, Args...>;
        using RingBuffer = detail::RingBuffer;

    public:
        static constexpr auto min_buffer_size = detail::min_buffer_size<FunctionContext>;

        FunctionQueue_SCSP(size_t buffer_size, allocator_type allocator = {}) noexcept
            : m_Writer{.buffer = detail::allocate_buffer<FunctionContext>(allocator, buffer_size),
                       .input_pos{m_Writer.buffer.data()},
                       .follow_pos{m_Writer.buffer.data()}},
              m_Reader{.output_pos{m_Writer.buffer.data()},
                       .buffer_begin{m_Writer.buffer.data()},
                       .buffer_end{m_Writer.buffer.data() + m_Writer.buffer.size()}},
              m_Allocator{allocator} {}

        ~FunctionQueue_SCSP() {
            if constexpr (destroyNonInvoked) {
                RingBuffer const ring_buffer{m_Writer.buffer, m_Writer.input_pos.load(std::memory_order_relaxed),
                                             m_Writer.follow_pos};
                detail::destroyAllNonInvoked<FunctionContext>(ring_buffer);
            }
            detail::deallocate_buffer<FunctionContext>(m_Allocator, m_Writer.buffer);
        }

        size_t buffer_size() const noexcept { return detail::buffer_size<FunctionContext>(m_Writer.buffer); }

        bool empty() const noexcept {
            return m_Writer.input_pos.load(std::memory_order::acquire) ==
                   m_Reader.output_pos.load(std::memory_order::relaxed);
        }

        template<typename... CallArgs>
        decltype(auto) call_and_pop(CallArgs &&...args) noexcept {
            auto const output_pos = m_Reader.output_pos.load(std::memory_order::relaxed);
            auto &functionCxt = *std::bit_cast<FunctionContext *>(output_pos);
            detail::ScopeGaurd const set_next_output_pos{[&, next_addr = output_pos + functionCxt.getStride()] {
                auto const next_pos = next_addr < m_Reader.buffer_end ? next_addr : m_Reader.buffer_begin;
                m_Reader.output_pos.store(next_pos, std::memory_order::release);
            }};
            return std::invoke(functionCxt, std::forward<CallArgs>(args)...);
        }

        template<typename T>
        bool push(T &&callable) noexcept {
            using Callable = std::remove_cvref_t<T>;
            return emplace<Callable>(std::forward<T>(callable));
        }

        template<typename Callable, typename... CArgs>
        requires detail::valid_callable<Callable, FunctionContext, CArgs...>
        bool emplace(CArgs &&...args) noexcept {
            RingBuffer const ring_buffer{m_Writer.buffer, m_Writer.input_pos.load(std::memory_order_relaxed),
                                         m_Writer.follow_pos};
            if (auto const next_addr =
                        detail::emplace<FunctionContext, Callable>(ring_buffer, std::forward<CArgs>(args)...)) {
                m_Writer.input_pos.store(next_addr, std::memory_order::release);
                return true;
            } else
                return false;
        }

        void clean_memory() noexcept { m_Writer.follow_pos = m_Reader.output_pos.load(std::memory_order::acquire); }

    private:
        alignas(detail::hardware_destructive_interference_size) struct {
            std::span<std::byte> const buffer;
            std::atomic<std::byte *> input_pos;
            std::byte *follow_pos;
        } m_Writer;

        alignas(detail::hardware_destructive_interference_size) struct {
            std::atomic<std::byte *> output_pos;
            std::byte *buffer_begin;
            std::byte *buffer_end;
        } m_Reader;

        allocator_type m_Allocator;
    };
}// namespace rb
#endif
