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
    template<typename T, size_t max_reader_threads, bool destroyNonInvoked = true,
             size_t max_obj_footprint = memory_footprint<std::aligned_storage_t<128>>>
    class FunctionQueue_MCSP;

    template<typename R, typename... Args, size_t max_reader_threads, bool destroyNonInvoked, size_t max_obj_footprint>
    class FunctionQueue_MCSP<R(Args...), max_reader_threads, destroyNonInvoked, max_obj_footprint> {
    private:
        using Offset = detail::TaggedUint32;
        using FunctionContext = detail::FunctionContext<destroyNonInvoked, max_obj_footprint, R, Args...>;
        using ReaderPos = std::atomic<Offset>;
        using ReaderPosPtrArray = std::array<ReaderPos *, max_reader_threads>;
        using CacheLine = std::aligned_storage_t<64, alignof(ReaderPos)>;

    public:
        class FunctionHandle {
        public:
            operator bool() const noexcept { return m_FcxtPtr; }

            template<typename... CArgs>
            decltype(auto) call_and_pop(CArgs &&...args) noexcept {
                detail::ScopeGaurd const free_function_cxt = [&] { m_FcxtPtr = nullptr; };
                return std::invoke(*m_FcxtPtr, std::forward<CArgs>(args)...);
            }

            FunctionHandle() noexcept {};

            FunctionHandle(FunctionHandle &&other) noexcept
                : m_FcxtPtr{std::exchange(other.m_FcxtPtr, nullptr)}, m_NextPos{other.m_NextPos} {}

            FunctionHandle &operator=(FunctionHandle &&other) noexcept {
                if (*this) destroy_callable();
                m_FcxtPtr = std::exchange(other.m_FcxtPtr, nullptr);
                m_NextPos = other.m_NextPos;
                return *this;
            }

            FunctionHandle(FunctionHandle const &) = delete;

            FunctionHandle &operator=(FunctionHandle const &other) = delete;

            ~FunctionHandle() {
                if (*this) destroy_callable();
            }

        private:
            FunctionHandle(FunctionContext *fcxt_ptr, Offset next_pos) noexcept
                : m_FcxtPtr{fcxt_ptr}, m_NextPos{next_pos} {}

            void destroy_callable() noexcept {
                if constexpr (destroyNonInvoked) m_FcxtPtr->destroyFO();
                m_FcxtPtr = nullptr;
            }

            friend class FunctionQueue_MCSP;
            FunctionContext *m_FcxtPtr{};
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
            Reader(FunctionQueue_MCSP const *functionQueue, ReaderPos *reader_pos) noexcept
                : m_FunctionQueue{functionQueue}, m_ReaderPos{reader_pos} {}

            friend class FunctionQueue_MCSP;

            FunctionQueue_MCSP const *m_FunctionQueue;
            ReaderPos *m_ReaderPos;
        };

        using allocator_type = std::pmr::polymorphic_allocator<>;

        static constexpr size_t min_buffer_size() noexcept { return FunctionContext::min_buffer_size; }

        FunctionQueue_MCSP(uint32_t buffer_size, uint16_t reader_threads, allocator_type allocator = {}) noexcept
            : m_BufferEndOffset{static_cast<uint32_t>(buffer_size - FunctionContext::sentinel_region_size)},
              m_ReaderThreads{reader_threads}, m_Buffer{static_cast<std::byte *>(allocator.allocate_bytes(
                                                       buffer_size, FunctionContext::buffer_alignment))},
              m_ReaderPosPtrArray{getReaderPosArray(allocator, reader_threads)}, m_Allocator{allocator} {}

        ~FunctionQueue_MCSP() {
            if constexpr (destroyNonInvoked) {
                clean_memory();
                FunctionContext::destroyAllNonInvoked(getBufferInfo());
            }
            m_Allocator.deallocate_object(m_Buffer, buffer_size());
            m_Allocator.deallocate_object(std::bit_cast<CacheLine *>(m_ReaderPosPtrArray[0]), m_ReaderThreads);
        }

        size_t buffer_size() const noexcept { return m_BufferEndOffset + FunctionContext::sentinel_region_size; }

        bool empty() const noexcept {
            return m_InputOffset.load(std::memory_order::acquire).value() ==
                   m_OutputReadOffset.load(std::memory_order::relaxed).value();
        }

        auto get_reader(uint16_t thread_index) const noexcept {
            auto const reader_pos = m_ReaderPosPtrArray[thread_index];
            reader_pos->store(m_OutputReadOffset.load(std::memory_order::relaxed), std::memory_order::relaxed);

            std::atomic_thread_fence(std::memory_order::release);
            return Reader{this, reader_pos};
        }

        template<typename T>
        bool push(T &&callable) noexcept {
            using Callable = std::decay_t<T>;
            return emplace<Callable>(std::forward<T>(callable));
        }

        template<typename Callable, typename... CArgs>
        requires FunctionContext::template is_valid_callable_v<Callable, CArgs...> bool
        emplace(CArgs &&...args) noexcept {
            if (auto const next_addr =
                        FunctionContext::template emplace<Callable>(getBufferInfo(), std::forward<CArgs>(args)...)) {
                auto const next_offset = static_cast<uint32_t>(next_addr - m_Buffer);
                auto const nextInputOffset = m_InputOffset.load(std::memory_order::relaxed).incr_tagged(next_offset);
                m_InputOffset.store(nextInputOffset, std::memory_order::release);

                if (nextInputOffset.tag() == 0) {
                    auto output_offset = nextInputOffset;
                    while (!m_OutputReadOffset.compare_exchange_weak(
                            output_offset, nextInputOffset.same_tagged(output_offset.value()),
                            std::memory_order::relaxed, std::memory_order::relaxed))
                        ;
                }
                return true;
            } else
                return false;
        }

        void clean_memory() noexcept {
            auto const currentFollowOffset = m_OutputFollowOffset;
            auto const currentReadOffset = m_OutputReadOffset.load(std::memory_order::acquire);

            auto less_value = [](auto l, auto r) { return l.value() < r.value(); };

            constexpr auto max_val = Offset::max();
            constexpr auto null_val = Offset::null();
            auto less_idx = max_val, gequal_idx = max_val;
            for (auto const &reader_pos : std::span{m_ReaderPosPtrArray.data(), m_ReaderThreads}) {
                if (auto const output_pos = reader_pos->load(std::memory_order::acquire); output_pos != null_val) {
                    if (output_pos.tag() <= currentFollowOffset.tag()) return;
                    else if (output_pos.value() >= currentFollowOffset.value())
                        gequal_idx = std::min(gequal_idx, output_pos, less_value);
                    else
                        less_idx = std::min(less_idx, output_pos, less_value);
                }
            }

            m_OutputFollowOffset =
                    (gequal_idx != max_val) ? gequal_idx : ((less_idx != max_val) ? less_idx : currentReadOffset);
        }

    private:
        FunctionHandle get_cxt() const noexcept {
            auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);
            FunctionContext *fcxt_ptr;
            Offset next_output_offset;

            do {
                if (input_offset.tag() < output_offset.tag() || input_offset.value() == output_offset.value())
                    return {};
                fcxt_ptr = std::bit_cast<FunctionContext *>(m_Buffer + output_offset.value());
                auto const next_offset = output_offset.value() + fcxt_ptr->getStride();
                next_output_offset = input_offset.same_tagged(next_offset < m_BufferEndOffset ? next_offset : 0);
            } while (!m_OutputReadOffset.compare_exchange_weak(output_offset, next_output_offset,
                                                               std::memory_order::relaxed, std::memory_order::relaxed));
            return {fcxt_ptr, next_output_offset};
        }

        FunctionHandle get_cxt_check_once() const noexcept {
            auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

            if (input_offset.tag() < output_offset.tag() || input_offset.value() == output_offset.value()) return {};

            auto const fcxt_ptr = std::bit_cast<FunctionContext *>(m_Buffer + output_offset.value());
            auto const next_offset = output_offset.value() + fcxt_ptr->getStride();
            auto const next_output_offset = input_offset.same_tagged(next_offset < m_BufferEndOffset ? next_offset : 0);

            if (m_OutputReadOffset.compare_exchange_strong(output_offset, next_output_offset,
                                                           std::memory_order::relaxed, std::memory_order::relaxed))
                return {fcxt_ptr, next_output_offset};
            else
                return {};
        }

        auto getBufferInfo() const noexcept {
            return detail::RingBufferInfo{.input_pos =
                                                  m_Buffer + m_InputOffset.load(std::memory_order::relaxed).value(),
                                          .output_pos = m_Buffer + m_OutputFollowOffset.value(),
                                          .buffer_start = m_Buffer,
                                          .buffer_end = m_Buffer + m_BufferEndOffset};
        }

        static auto getReaderPosArray(allocator_type allocator, uint16_t threads) noexcept {
            ReaderPosPtrArray readerPosArray;
            auto const cache_line_array = allocator.allocate_object<CacheLine>(threads);
            for (uint16_t t{0}; t != threads; ++t) {
                auto const reader_pos_ptr = std::bit_cast<ReaderPos *>(cache_line_array + t);
                std::construct_at(reader_pos_ptr, Offset::null());
                readerPosArray[t] = reader_pos_ptr;
            }
            std::atomic_thread_fence(std::memory_order::release);
            return readerPosArray;
        }

    private:
        std::atomic<Offset> m_InputOffset{};
        Offset m_OutputFollowOffset{};
        mutable std::atomic<Offset> m_OutputReadOffset{};

        uint32_t const m_BufferEndOffset;
        uint32_t const m_ReaderThreads;
        std::byte *const m_Buffer;
        ReaderPosPtrArray const m_ReaderPosPtrArray;
        allocator_type m_Allocator;
    };
}// namespace rb
#endif
