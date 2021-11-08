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
#include <type_traits>
#include <utility>

namespace rb {
    template<typename T, size_t max_reader_threads, bool destroyNonInvoked = true,
             size_t max_obj_footprint = memory_footprint<std::aligned_storage_t<128>>>
    class FunctionQueue_MCSP;

    template<typename R, typename... Args, size_t max_reader_threads, bool destroyNonInvoked, size_t max_obj_footprint>
    class FunctionQueue_MCSP<R(Args...), max_reader_threads, destroyNonInvoked, max_obj_footprint> {
    private:
        using FunctionContextType = detail::FunctionContext<destroyNonInvoked, max_obj_footprint, R, Args...>;
        using ReaderPos = std::atomic<std::byte *>;
        using ReaderPosPtrArray = std::array<ReaderPos *, max_reader_threads>;
        using CacheLine = std::aligned_storage_t<64, alignof(ReaderPos)>;

    public:
        class FunctionHandle {
        public:
            FunctionHandle() noexcept {};

            FunctionHandle(FunctionHandle &&other) noexcept
                : m_FcxtPtr{std::exchange(other.m_FcxtPtr, nullptr)}, m_NextAddr{other.m_NextAddr} {}

            operator bool() const noexcept { return m_FcxtPtr; }

            template<typename... CArgs>
            decltype(auto) call_and_pop(CArgs &&...args) noexcept {
                detail::ScopeGaurd const free_function_cxt = [&] { m_FcxtPtr = nullptr; };
                return std::invoke(*m_FcxtPtr, std::forward<CArgs>(args)...);
            }

            FunctionHandle &operator=(FunctionHandle &&other) noexcept {
                if (*this) destroy_callable();
                m_FcxtPtr = std::exchange(other.m_FcxtPtr, nullptr);
                m_NextAddr = other.m_NextAddr;
                return *this;
            }

            FunctionHandle(FunctionHandle const &) = delete;

            FunctionHandle &operator=(FunctionHandle const &other) = delete;

            ~FunctionHandle() {
                if (*this) destroy_callable();
            }

        private:
            FunctionHandle(FunctionContextType *fcxt_ptr, std::byte *next_addr) noexcept
                : m_FcxtPtr{fcxt_ptr}, m_NextAddr{next_addr} {}

            void destroy_callable() noexcept {
                if constexpr (destroyNonInvoked) m_FcxtPtr->destroyFO();
                m_FcxtPtr = nullptr;
            }

            friend class FunctionQueue_MCSP;
            FunctionContextType *m_FcxtPtr{};
            std::byte *m_NextAddr;
        };

        class Reader {
        public:
            void release(FunctionHandle &&h) const noexcept {
                detail::ScopeGaurd const rs = [&, next_addr{h.m_NextAddr}] {
                    m_ReaderPos->store(next_addr, std::memory_order::release);
                };
                auto const handle = std::move(h);
            }

            auto get_function_handle() const noexcept { return m_FunctionQueue->get_cxt(); }

            auto get_function_handle(check_once_tag) const noexcept { return m_FunctionQueue->get_cxt_check_once(); }

            ~Reader() { m_ReaderPos->store(nullptr, std::memory_order::release); }

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

        static constexpr size_t min_buffer_size() noexcept { return FunctionContextType::min_buffer_size; }

        FunctionQueue_MCSP(uint32_t buffer_size, uint16_t reader_threads, allocator_type allocator = {}) noexcept
            : m_BufferEndOffset{static_cast<uint32_t>(buffer_size - FunctionContextType::sentinel_region_size)},
              m_ReaderThreads{reader_threads}, m_Buffer{static_cast<std::byte *>(allocator.allocate_bytes(
                                                       buffer_size, FunctionContextType::buffer_alignment))},
              m_ReaderPosPtrArray{getReaderPosArray(allocator, reader_threads)}, m_Allocator{allocator} {}

        ~FunctionQueue_MCSP() {
            if constexpr (destroyNonInvoked) FunctionContextType::destroyAllNonInvoked(getBufferInfo());
            m_Allocator.deallocate_object(m_Buffer, buffer_size());
            m_Allocator.deallocate_object(std::bit_cast<CacheLine *>(m_ReaderPosPtrArray[0]), m_ReaderThreads);
        }

        size_t buffer_size() const noexcept { return m_BufferEndOffset + FunctionContextType::sentinel_region_size; }

        bool empty() const noexcept {
            return m_InputOffset.load(std::memory_order::acquire).getValue() ==
                   m_OutputReadOffset.load(std::memory_order::relaxed).getValue();
        }

        auto getReader(uint16_t thread_index) const noexcept {
            auto const reader_pos = m_ReaderPosPtrArray[thread_index];
            reader_pos->store(m_Buffer + m_OutputReadOffset.load(std::memory_order::relaxed).getValue(),
                              std::memory_order::relaxed);

            std::atomic_thread_fence(std::memory_order::release);
            return Reader{this, reader_pos};
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
                auto const next_offset = static_cast<uint32_t>(next_addr - m_Buffer);
                auto const nextInputOffset = m_InputOffset.load(std::memory_order::relaxed).getIncrTagged(next_offset);
                m_InputOffset.store(nextInputOffset, std::memory_order::release);

                if (nextInputOffset.getTag() == 0) {
                    auto output_offset = nextInputOffset;
                    while (!m_OutputReadOffset.compare_exchange_weak(
                            output_offset, nextInputOffset.getSameTagged(output_offset.getValue()),
                            std::memory_order::relaxed, std::memory_order::relaxed))
                        ;
                }
                return true;
            } else
                return false;
        }

        void clean_memory() noexcept {
            constexpr auto MAX_IDX = std::numeric_limits<uint32_t>::max();
            auto const currentFollowIndex = m_OutputFollowOffset;
            auto const currentReadIndex = m_OutputReadOffset.load(std::memory_order::acquire).getValue();

            auto less_idx = MAX_IDX, gequal_idx = MAX_IDX;
            for (auto const &reader_pos : std::span{m_ReaderPosPtrArray.data(), m_ReaderThreads}) {
                if (auto const output_pos = reader_pos->load(std::memory_order::acquire)) {
                    auto const output_index = static_cast<uint32_t>(output_pos - m_Buffer);
                    if (output_index >= currentFollowIndex) gequal_idx = std::min(gequal_idx, output_index);
                    else
                        less_idx = std::min(less_idx, output_index);
                }
            }

            m_OutputFollowOffset =
                    (gequal_idx != MAX_IDX) ? gequal_idx : ((less_idx != MAX_IDX) ? less_idx : currentReadIndex);
        }

    private:
        FunctionHandle get_cxt() const noexcept {
            auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);
            FunctionContextType *fcxt_ptr;
            detail::TaggedUint32 next_output_offset;

            do {
                if (input_offset.getTag() < output_offset.getTag() ||
                    input_offset.getValue() == output_offset.getValue())
                    return {};
                fcxt_ptr = std::bit_cast<FunctionContextType *>(m_Buffer + output_offset.getValue());
                auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
                next_output_offset = input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);
            } while (!m_OutputReadOffset.compare_exchange_weak(output_offset, next_output_offset,
                                                               std::memory_order::relaxed, std::memory_order::relaxed));
            return {fcxt_ptr, m_Buffer + next_output_offset.getValue()};
        }

        FunctionHandle get_cxt_check_once() const noexcept {
            auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

            if (input_offset.getTag() < output_offset.getTag() || input_offset.getValue() == output_offset.getValue())
                return {};

            auto const fcxt_ptr = std::bit_cast<FunctionContextType *>(m_Buffer + output_offset.getValue());
            auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
            auto const next_output_offset =
                    input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);

            if (m_OutputReadOffset.compare_exchange_strong(output_offset, next_output_offset,
                                                           std::memory_order::relaxed, std::memory_order::relaxed))
                return {fcxt_ptr, m_Buffer + next_output_offset.getValue()};
            else
                return {};
        }

        auto getBufferInfo() const noexcept {
            return detail::RingBufferInfo{.input_pos =
                                                  m_Buffer + m_InputOffset.load(std::memory_order::relaxed).getValue(),
                                          .output_pos = m_Buffer + m_OutputFollowOffset,
                                          .buffer_start = m_Buffer,
                                          .buffer_end = m_Buffer + m_BufferEndOffset};
        }

        static auto getReaderPosArray(allocator_type allocator, uint16_t threads) noexcept {
            ReaderPosPtrArray readerPosArray;
            auto const cache_line_array = allocator.allocate_object<CacheLine>(threads);
            for (uint16_t t{0}; t != threads; ++t) {
                auto const reader_pos_ptr = std::bit_cast<ReaderPos *>(cache_line_array + t);
                reader_pos_ptr->store(nullptr, std::memory_order_release);
                readerPosArray[t] = reader_pos_ptr;
            }
            return readerPosArray;
        }

    private:
        static constexpr uint32_t INVALID_INDEX{std::numeric_limits<uint32_t>::max()};

        std::atomic<detail::TaggedUint32> m_InputOffset{};
        uint32_t m_OutputFollowOffset{};
        mutable std::atomic<detail::TaggedUint32> m_OutputReadOffset{};

        uint32_t const m_BufferEndOffset;
        uint32_t const m_ReaderThreads;
        std::byte *const m_Buffer;
        ReaderPosPtrArray const m_ReaderPosPtrArray;
        allocator_type m_Allocator;
    };
}// namespace rb
#endif
