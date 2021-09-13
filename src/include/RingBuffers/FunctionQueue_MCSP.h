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
#include <type_traits>
#include <utility>

namespace rb {
    template<typename T, size_t max_reader_threads, bool destroyNonInvoked = true,
             size_t max_obj_footprint = memory_footprint<std::aligned_storage_t<128>>>
    class FunctionQueue_MCSP {};

    template<typename R, typename... Args, size_t max_reader_threads, bool destroyNonInvoked, size_t max_obj_footprint>
    class FunctionQueue_MCSP<R(Args...), max_reader_threads, destroyNonInvoked, max_obj_footprint> {
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

        using CxtPtr = std::pair<FunctionContextType *, std::byte *>;
        using ReaderPos = std::atomic<std::byte *>;
        using ReaderPosPtrArray = std::array<ReaderPos *, max_reader_threads>;
        using CacheLine = std::aligned_storage_t<64>;

    public:
        using allocator_type = std::pmr::polymorphic_allocator<>;

        class FunctionHandle {
        public:
            FunctionHandle() noexcept = default;

            FunctionHandle(FunctionHandle &&other) noexcept
                : cxt_ptr{std::exchange(other.cxt_ptr, {nullptr, nullptr})}, reader_pos{other.reader_pos} {}

            FunctionHandle(FunctionHandle const &) = delete;

            FunctionHandle &operator=(FunctionHandle const &other) = delete;

            operator bool() const noexcept { return cxt_ptr.first; }

            R call_and_pop(Args... args) noexcept {
                detail::ScopeGaurd const free_function_cxt = {[&] {
                    reader_pos->store(cxt_ptr.second, std::memory_order::release);
                    cxt_ptr.first = nullptr;
                }};

                return (*cxt_ptr.first) (std::forward<Args>(args)...);
            }

            FunctionHandle &operator=(FunctionHandle &&other) noexcept {
                if (*this) release();
                cxt_ptr = std::exchange(other.cxt_ptr, {nullptr, nullptr});
                reader_pos = other.reader_pos;
                return *this;
            }

            ~FunctionHandle() {
                if (*this) release();
            }

        private:
            FunctionHandle(CxtPtr cxt_ptr, ReaderPos *reader_pos) noexcept : cxt_ptr{cxt_ptr}, reader_pos{reader_pos} {}

            void release() {
                if constexpr (destroyNonInvoked) cxt_ptr.first->destroyFO();
                reader_pos->store(cxt_ptr.second, std::memory_order::release);
            }

            friend class FunctionQueue_MCSP;
            CxtPtr cxt_ptr{nullptr, nullptr};
            ReaderPos *reader_pos;
        };

        class Reader {
        public:
            auto get_function_handle() const noexcept {
                return FunctionHandle{m_FunctionQueue->get_cxt(), m_ReaderPos};
            }

            auto get_function_handle(check_once_tag) const noexcept {
                return FunctionHandle{m_FunctionQueue->get_cxt_check_once(), m_ReaderPos};
            }

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

        FunctionQueue_MCSP(uint32_t buffer_size, uint16_t reader_threads, allocator_type allocator = {}) noexcept
            : m_BufferEndOffset{static_cast<uint32_t>(buffer_size - sentinel_region_size)},
              m_ReaderThreads{reader_threads}, m_Buffer{static_cast<std::byte *>(
                                                       allocator.allocate_bytes(buffer_size, buffer_alignment))},
              m_ReaderPosArray{getReaderPosArray(allocator, reader_threads)}, m_Allocator{allocator} {
            resetReaderPosArray();
        }

        ~FunctionQueue_MCSP() {
            if constexpr (destroyNonInvoked) destroyAllNonInvoked();
            m_Allocator.deallocate_object(m_Buffer, buffer_size());
            m_Allocator.deallocate_object(std::bit_cast<CacheLine *>(m_ReaderPosArray[0]), m_ReaderThreads);
        }

        size_t buffer_size() const noexcept { return m_BufferEndOffset + sentinel_region_size; }

        bool empty() const noexcept {
            return m_InputOffset.load(std::memory_order::acquire).getValue() ==
                   m_OutputReadOffset.load(std::memory_order::relaxed).getValue();
        }

        void clear() noexcept {
            if constexpr (destroyNonInvoked) destroyAllNonInvoked();

            m_InputOffset.store({}, std::memory_order::relaxed);
            m_OutputFollowOffset = 0;
            m_OutputReadOffset.store({}, std::memory_order::relaxed);
            resetReaderPosArray();
        }

        auto getReader(uint16_t thread_index) const noexcept {
            auto const reader_pos = m_ReaderPosArray[thread_index];
            reader_pos->store(m_Buffer + m_OutputReadOffset.load(std::memory_order::relaxed).getValue(),
                              std::memory_order::release);
            return Reader{this, reader_pos};
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
            for (uint16_t t = 0; t != m_ReaderThreads; ++t) {
                if (auto const output_pos = m_ReaderPosArray[t]->load(std::memory_order::acquire)) {
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
        CxtPtr get_cxt() const noexcept {
            constexpr auto null_cxt = CxtPtr{nullptr, nullptr};
            auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);
            FunctionContextType *fcxt_ptr;
            detail::TaggedUint32 next_output_offset;

            do {
                if (input_offset.getTag() < output_offset.getTag() ||
                    input_offset.getValue() == output_offset.getValue())
                    return null_cxt;
                fcxt_ptr = std::bit_cast<FunctionContextType *>(m_Buffer + output_offset.getValue());
                auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
                next_output_offset = input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);
            } while (!m_OutputReadOffset.compare_exchange_weak(output_offset, next_output_offset,
                                                               std::memory_order::relaxed, std::memory_order::relaxed));
            return {fcxt_ptr, m_Buffer + next_output_offset.getValue()};
        }

        CxtPtr get_cxt_check_once() const noexcept {
            constexpr auto null_cxt = CxtPtr{nullptr, nullptr};
            auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

            if (input_offset.getTag() < output_offset.getTag() || input_offset.getValue() == output_offset.getValue())
                return null_cxt;

            auto const fcxt_ptr = std::bit_cast<FunctionContextType *>(m_Buffer + output_offset.getValue());
            auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
            auto const next_output_offset =
                    input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);

            if (m_OutputReadOffset.compare_exchange_strong(output_offset, next_output_offset,
                                                           std::memory_order::relaxed, std::memory_order::relaxed))
                return {fcxt_ptr, m_Buffer + next_output_offset.getValue()};
            else
                return null_cxt;
        }

        auto getBufferInfo() const noexcept {
            return detail::RingBufferInfo{.input_pos =
                                                  m_Buffer + m_InputOffset.load(std::memory_order::relaxed).getValue(),
                                          .output_pos = m_Buffer + m_OutputFollowOffset,
                                          .buffer_start = m_Buffer,
                                          .buffer_end = m_Buffer + m_BufferEndOffset};
        }

        void destroyAllNonInvoked() noexcept { detail::destroyAllNonInvoked<FunctionContextType>(getBufferInfo()); }

        void resetReaderPosArray() noexcept {
            for (uint16_t t = 0; t != m_ReaderThreads; ++t)
                m_ReaderPosArray[t]->store(nullptr, std::memory_order::relaxed);
        }

        static auto getReaderPosArray(allocator_type allocator, uint16_t threads) noexcept {
            ReaderPosPtrArray readerPosArray;
            auto const cache_line_array = allocator.allocate_object<CacheLine>(threads);
            for (uint16_t t = 0; t != threads; ++t)
                readerPosArray[t] = std::bit_cast<ReaderPos *>(cache_line_array + t);
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
        ReaderPosPtrArray const m_ReaderPosArray;
        allocator_type m_Allocator;
    };
}// namespace rb
#endif
