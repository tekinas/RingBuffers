#ifndef FUNCTIONQUEUE_MCSP
#define FUNCTIONQUEUE_MCSP

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
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

namespace rb {
    template<typename CallableSignature, bool destroyNonInvoked = true,
             size_t max_obj_footprint = memory_footprint<std::aligned_storage_t<128>>>
    class FunctionQueue_MCSP;

    template<typename R, typename... Args, bool destroyNonInvoked, size_t max_obj_footprint>
    class FunctionQueue_MCSP<R(Args...), destroyNonInvoked, max_obj_footprint> {
    private:
        using Offset = detail::TaggedUint32;
        using FunctionContext = detail::FunctionContext<destroyNonInvoked, max_obj_footprint, R, Args...>;
        using ReaderPos = std::atomic<Offset>;
        using CacheLine = std::aligned_storage_t<detail::hardware_destructive_interference_size>;
        using RingBuffer = detail::RingBuffer;

    public:
        class FunctionHandle {
        public:
            operator bool() const noexcept { return m_FcxtPtr; }

            template<typename... CArgs>
            decltype(auto) call_and_pop(CArgs &&...args) noexcept {
                detail::ScopeGaurd const free_function_cxt = [&] { m_FcxtPtr = nullptr; };
                return std::invoke(*m_FcxtPtr, std::forward<CArgs>(args)...);
            }

            FunctionHandle() noexcept : m_FcxtPtr{} {}

            FunctionHandle(FunctionHandle &&other) noexcept
                : m_FcxtPtr{std::exchange(other.m_FcxtPtr, nullptr)}, m_NextPos{other.m_NextPos} {}

            FunctionHandle &operator=(FunctionHandle &&other) noexcept {
                auto tmp = std::move(other);
                std::swap(m_FcxtPtr, tmp.m_FcxtPtr);
                m_NextPos = tmp.m_NextPos;
                return *this;
            }

            ~FunctionHandle() requires destroyNonInvoked {
                if (*this) m_FcxtPtr->destroyFO();
            }

            ~FunctionHandle() = default;

            friend class FunctionQueue_MCSP;

        private:
            FunctionHandle(FunctionContext *fcxt_ptr, Offset next_pos) noexcept
                : m_FcxtPtr{fcxt_ptr}, m_NextPos{next_pos} {}

            FunctionContext *m_FcxtPtr;
            Offset m_NextPos;
        };

        class Reader {
        public:
            void release(FunctionHandle &&h) const noexcept {
                detail::ScopeGaurd const rs = [&, next_pos{h.m_NextPos}] {
                    m_ReaderPos->store(next_pos, std::memory_order::release);
                };
                auto const handle = std::move(h);
            }

            auto get_function_handle() const noexcept { return m_FunctionQueue->get_cxt(); }

            auto get_function_handle(check_once_tag) const noexcept { return m_FunctionQueue->get_cxt_check_once(); }

            ~Reader() { m_ReaderPos->store(Offset::null(), std::memory_order::release); }

            Reader(Reader const &) = delete;
            Reader &operator=(Reader const &) = delete;
            Reader(Reader &&) = delete;
            Reader &operator=(Reader &&) = delete;

        private:
            Reader(FunctionQueue_MCSP *functionQueue, ReaderPos *reader_pos) noexcept
                : m_FunctionQueue{functionQueue}, m_ReaderPos{reader_pos} {}

            friend class FunctionQueue_MCSP;

            FunctionQueue_MCSP *m_FunctionQueue;
            ReaderPos *m_ReaderPos;
        };

        static constexpr size_t min_buffer_size() noexcept { return FunctionContext::min_buffer_size; }

        FunctionQueue_MCSP(size_t buffer_size, uint16_t reader_threads, allocator_type allocator = {}) noexcept
            : m_Writer{.input_offset{},
                       .follow_offset{},
                       .buffer = detail::allocate_buffer<FunctionContext>(allocator, buffer_size)},
              m_Reader{.output_offset{},
                       .buffer = m_Writer.buffer,
                       .position_array = allocate_pos_array(allocator, reader_threads)},
              m_Allocator{allocator} {}

        ~FunctionQueue_MCSP() {
            if constexpr (destroyNonInvoked) {
                clean_memory();
                RingBuffer const ring_buffer{m_Writer.buffer,
                                             m_Writer.input_offset.load(std::memory_order::relaxed).value(),
                                             m_Writer.follow_offset.value()};
                detail::destroyAllNonInvoked<FunctionContext>(ring_buffer);
            }
            detail::deallocate_buffer<FunctionContext>(m_Allocator, m_Writer.buffer);
            m_Allocator.deallocate_object(m_Reader.position_array.data(), m_Reader.position_array.size());
        }

        size_t buffer_size() const noexcept { return detail::buffer_size<FunctionContext>(m_Writer.buffer); }

        bool empty() const noexcept {
            return m_Writer.input_offset.load(std::memory_order::acquire).value() ==
                   m_Reader.output_offset.load(std::memory_order::relaxed).value();
        }

        auto get_reader(uint16_t thread_index) noexcept {
            auto const reader_pos = reinterpret_cast<ReaderPos *>(&m_Reader.position_array[thread_index]);
            reader_pos->store(m_Reader.output_offset.load(std::memory_order::relaxed), std::memory_order::relaxed);

            std::atomic_thread_fence(std::memory_order::release);
            return Reader{this, reader_pos};
        }

        template<typename T>
        bool push(T &&callable) noexcept {
            using Callable = std::decay_t<T>;
            return emplace<Callable>(std::forward<T>(callable));
        }

        template<typename Callable, typename... CArgs>
        requires detail::valid_callable<Callable, FunctionContext, CArgs...>
        bool emplace(CArgs &&...args) noexcept {
            auto const input_offset = m_Writer.input_offset.load(std::memory_order::relaxed);
            RingBuffer const ring_buffer{m_Writer.buffer, input_offset.value(), m_Writer.follow_offset.value()};

            if (auto const next_addr =
                        detail::emplace<FunctionContext, Callable>(ring_buffer, std::forward<CArgs>(args)...)) {
                auto const next_offset = static_cast<uint32_t>(next_addr - m_Writer.buffer.data());
                auto const next_input_offset = input_offset.incr_tagged(next_offset);
                m_Writer.input_offset.store(next_input_offset, std::memory_order::release);

                if (next_input_offset.tag() == 0) {
                    auto output_offset = next_input_offset;
                    while (!m_Reader.output_offset.compare_exchange_weak(
                            output_offset, next_input_offset.same_tagged(output_offset.value()),
                            std::memory_order::relaxed, std::memory_order::relaxed))
                        ;
                }
                return true;
            } else
                return false;
        }

        void clean_memory() noexcept {
            auto const currentFollowOffset = m_Writer.follow_offset;
            auto const currentReadOffset = m_Reader.output_offset.load(std::memory_order::acquire);

            auto less_value = [](auto l, auto r) { return l.value() < r.value(); };

            constexpr auto max_val = Offset::max();
            constexpr auto null_val = Offset::null();
            auto less_idx = max_val, gequal_idx = max_val;
            for (auto const &cache_line : m_Reader.position_array) {
                auto const &reader_pos = reinterpret_cast<ReaderPos const &>(cache_line);
                if (auto const output_pos = reader_pos.load(std::memory_order::relaxed); output_pos != null_val) {
                    if (output_pos.tag() <= currentFollowOffset.tag()) return;
                    else if (output_pos.value() >= currentFollowOffset.value())
                        gequal_idx = std::min(gequal_idx, output_pos, less_value);
                    else
                        less_idx = std::min(less_idx, output_pos, less_value);
                }
            }

            m_Writer.follow_offset =
                    (gequal_idx != max_val) ? gequal_idx : ((less_idx != max_val) ? less_idx : currentReadOffset);

            std::atomic_thread_fence(std::memory_order::acquire);
        }

    private:
        FunctionHandle get_cxt() noexcept {
            auto output_offset = m_Reader.output_offset.load(std::memory_order::relaxed);
            auto const buffer = m_Reader.buffer;
            auto const input_offset = m_Writer.input_offset.load(std::memory_order::acquire);
            FunctionContext *fcxt_ptr;
            Offset next_output_offset;

            do {
                if (input_offset.tag() < output_offset.tag() || input_offset.value() == output_offset.value())
                    return {};
                fcxt_ptr = reinterpret_cast<FunctionContext *>(&buffer[output_offset.value()]);
                auto const next_offset = output_offset.value() + fcxt_ptr->getStride();
                next_output_offset = input_offset.same_tagged(next_offset < buffer.size() ? next_offset : 0);
            } while (!m_Reader.output_offset.compare_exchange_weak(
                    output_offset, next_output_offset, std::memory_order::relaxed, std::memory_order::relaxed));
            return {fcxt_ptr, next_output_offset};
        }

        FunctionHandle get_cxt_check_once() noexcept {
            auto output_offset = m_Reader.output_offset.load(std::memory_order::relaxed);
            auto const input_offset = m_Writer.input_offset.load(std::memory_order::acquire);

            if (input_offset.tag() < output_offset.tag() || input_offset.value() == output_offset.value()) return {};

            auto const buffer = m_Reader.buffer;
            auto const fcxt_ptr = reinterpret_cast<FunctionContext *>(&buffer[output_offset.value()]);
            auto const next_offset = output_offset.value() + fcxt_ptr->getStride();
            auto const next_output_offset = input_offset.same_tagged(next_offset < buffer.size() ? next_offset : 0);

            if (m_Reader.output_offset.compare_exchange_strong(output_offset, next_output_offset,
                                                               std::memory_order::relaxed, std::memory_order::relaxed))
                return {fcxt_ptr, next_output_offset};
            else
                return {};
        }

        static auto allocate_pos_array(allocator_type allocator, uint16_t threads) noexcept {
            auto const cache_line_array = std::span{allocator.allocate_object<CacheLine>(threads), threads};
            for (auto &cache_line : cache_line_array) {
                auto const reader_pos_ptr = reinterpret_cast<ReaderPos *>(&cache_line);
                std::construct_at(reader_pos_ptr, Offset::null());
            }
            return cache_line_array;
        }

    private:
        alignas(detail::hardware_destructive_interference_size) struct {
            std::atomic<Offset> input_offset;
            Offset follow_offset;
            std::span<std::byte> const buffer;
        } m_Writer;

        alignas(detail::hardware_destructive_interference_size) struct {
            std::atomic<Offset> output_offset;
            std::span<std::byte> const buffer;
            std::span<CacheLine> const position_array;
        } m_Reader;

        allocator_type m_Allocator;
    };
}// namespace rb
#endif
