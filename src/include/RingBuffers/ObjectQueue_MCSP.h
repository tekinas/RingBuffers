#ifndef OBJECTQUEUE_MCSP
#define OBJECTQUEUE_MCSP

#include "detail/rb_detail.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <type_traits>
#include <utility>

namespace rb {
    template<typename ObjectType, size_t max_reader_threads>
    requires std::is_nothrow_destructible_v<ObjectType>
    class ObjectQueue_MCSP {
    private:
        static_assert(max_reader_threads != 0);

        using ReaderPos = std::atomic<ObjectType *>;
        using ReaderPosPtrArray = std::array<ReaderPos *, max_reader_threads>;
        using CacheLine = std::aligned_storage_t<64, alignof(ReaderPos)>;

    public:
        class Ptr {
        public:
            operator bool() const noexcept { return m_ObjectPtr; }

            ObjectType &operator*() const noexcept { return *m_ObjectPtr; }

            ObjectType *get() const noexcept { return m_ObjectPtr; }

            ObjectType *operator->() const noexcept { return m_ObjectPtr; }

            Ptr() noexcept {}

            Ptr(Ptr &&other) noexcept
                : m_ObjectPtr{std::exchange(other.m_ObjectPtr, nullptr)}, m_NextPtr{other.m_NextPtr} {}

            Ptr &operator=(Ptr &&other) noexcept {
                if (*this) destroy_object();

                m_ObjectPtr = std::exchange(other.m_ObjectPtr, nullptr);
                m_NextPtr = other.m_NextPtr;
                return *this;
            }

            Ptr(Ptr const &) = delete;

            Ptr &operator=(Ptr const &) = delete;

            ~Ptr() {
                if (*this) destroy_object();
            }

        private:
            Ptr(ObjectType *object_pos, ObjectType *next_pos) noexcept : m_ObjectPtr{object_pos}, m_NextPtr{next_pos} {}

            void destroy_object() noexcept { std::destroy_at(std::exchange(m_ObjectPtr, nullptr)); }

            friend class ObjectQueue_MCSP;
            ObjectType *m_ObjectPtr{};
            ObjectType *m_NextPtr;
        };

        class Reader {
        public:
            void release(Ptr &&p) const noexcept {
                detail::ScopeGaurd const rs = [&, next_pos{p.m_NextPtr}] {
                    m_ReaderPos->store(next_pos, std::memory_order::release);
                };
                auto const ptr = std::move(p);
            }

            Ptr consume() const noexcept { return m_ObjectQueue->get_object(); }

            Ptr consume(rb::check_once_tag) const noexcept { return m_ObjectQueue->get_object_check_once(); }

            template<typename Functor>
            requires std::is_nothrow_invocable_v<Functor, ObjectType &>
            bool consume(Functor &&functor) const noexcept {
                return consume_impl<&ObjectQueue_MCSP::get_object>(std::forward<Functor>(functor));
            }

            template<typename Functor>
            requires std::is_nothrow_invocable_v<Functor, ObjectType &>
            bool consume(rb::check_once_tag, Functor &&functor) const noexcept {
                return consume_impl<&ObjectQueue_MCSP::get_object_check_once>(std::forward<Functor>(functor));
            }

            template<typename Functor>
            requires std::is_nothrow_invocable_v<Functor, ObjectType &> uint32_t consume_all(Functor &&functor)
            const noexcept { return consume_all_impl<&ObjectQueue_MCSP::get_object>(std::forward<Functor>(functor)); }

            template<typename Functor>
            requires std::is_nothrow_invocable_v<Functor, ObjectType &> uint32_t consume_all(rb::check_once_tag,
                                                                                             Functor &&functor)
            const noexcept {
                return consume_all_impl<&ObjectQueue_MCSP::get_object_check_once>(std::forward<Functor>(functor));
            }

            ~Reader() { m_ReaderPos->store(nullptr, std::memory_order::release); }

            Reader(Reader const &) = delete;
            Reader &operator=(Reader const &) = delete;
            Reader(Reader &&) = delete;
            Reader &operator=(Reader &&) = delete;

        private:
            template<auto get_object, typename Functor>
            bool consume_impl(Functor &&functor) const noexcept {
                if (auto ptr = std::invoke(get_object, m_ObjectQueue)) {
                    std::invoke(std::forward<Functor>(functor), *ptr);
                    release(std::move(ptr));
                    return true;
                } else
                    return false;
            }

            template<auto get_object, typename Functor>
            uint32_t consume_all_impl(Functor &&functor) const noexcept {
                uint32_t consumed{0};
                while (auto const ptr = std::invoke(get_object, m_ObjectQueue)) {
                    std::invoke(std::forward<Functor>(functor), *ptr);
                    ++consumed;
                }
                return consumed;
            }

            Reader(ObjectQueue_MCSP const *objectQueue, ReaderPos *reader_pos) noexcept
                : m_ObjectQueue{objectQueue}, m_ReaderPos{reader_pos} {}

        private:
            friend class ObjectQueue_MCSP;
            ObjectQueue_MCSP const *m_ObjectQueue{};
            ReaderPos *m_ReaderPos;
        };

        using allocator_type = std::pmr::polymorphic_allocator<>;

        ObjectQueue_MCSP(uint32_t array_size, uint16_t reader_threads, allocator_type allocator = {}) noexcept
            : m_LastElementIndex{array_size},
              m_ReaderThreads{reader_threads}, m_Array{allocator.allocate_object<ObjectType>(array_size + 1)},
              m_ReaderPosPtrArray{getReaderPosArray(allocator, reader_threads)}, m_Allocator{allocator} {}

        ~ObjectQueue_MCSP() {
            destroyAllObjects();

            m_Allocator.deallocate_object(m_Array, m_LastElementIndex + 1);
            m_Allocator.deallocate_object(std::bit_cast<CacheLine *>(m_ReaderPosPtrArray[0]), m_ReaderThreads);
        }

        uint32_t array_size() const noexcept { return m_LastElementIndex; }

        bool empty() const noexcept {
            return m_InputIndex.load(std::memory_order::acquire).getValue() ==
                   m_OutputReadIndex.load(std::memory_order::acquire).getValue();
        }

        Reader getReader(uint16_t thread_index) const noexcept {
            auto const reader_pos = m_ReaderPosPtrArray[thread_index];
            reader_pos->store(m_Array + m_OutputReadIndex.load(std::memory_order::relaxed).getValue(),
                              std::memory_order::relaxed);
            std::atomic_thread_fence(std::memory_order::release);

            return Reader{this, reader_pos};
        }

        bool push(ObjectType const &obj) noexcept { return emplace(obj); }

        bool push(ObjectType &&obj) noexcept { return emplace(std::move(obj)); }

        template<typename... Args>
        requires std::is_nothrow_constructible_v<ObjectType, Args...>
        bool emplace(Args &&...args) noexcept {
            auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
            auto const index_value = input_index.getValue();
            auto const output_index = m_OutputFollowIndex;
            auto const nextInputIndexValue = index_value == m_LastElementIndex ? 0 : (index_value + 1);

            if (nextInputIndexValue == output_index) return false;

            std::construct_at(m_Array + index_value, std::forward<Args>(args)...);

            auto const nextInputIndex = input_index.getIncrTagged(nextInputIndexValue);
            m_InputIndex.store(nextInputIndex, std::memory_order::release);

            if (nextInputIndex.getTag() == 0) [[unlikely]] {
                auto output_offset = nextInputIndex;
                while (!m_OutputReadIndex.compare_exchange_weak(output_offset,
                                                                nextInputIndex.getSameTagged(output_offset.getValue()),
                                                                std::memory_order::relaxed, std::memory_order::relaxed))
                    ;
            }

            return true;
        }

        template<typename Functor>
        requires std::is_nothrow_invocable_r_v<uint32_t, Functor, ObjectType *, uint32_t>
                uint32_t emplace_back_n(Functor &&functor)
        noexcept {
            auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
            auto const index_value = input_index.getValue();
            auto const output_index = m_OutputFollowIndex;
            auto const count_avl = (index_value < output_index)
                                           ? (output_index - index_value - 1)
                                           : (m_LastElementIndex - index_value + static_cast<bool>(output_index));

            if (!count_avl) return 0;

            auto const obj_emplaced = std::forward<Functor>(functor)(m_Array + index_value, count_avl);
            auto const input_end = index_value + obj_emplaced;
            auto const nextInputIndexValue = (input_end == (m_LastElementIndex + 1)) ? 0 : input_end;
            auto const nextInputIndex = input_index.getIncrTagged(nextInputIndexValue);

            m_InputIndex.store(nextInputIndex, std::memory_order::release);

            if (nextInputIndex.getTag() == 0) {
                auto output_offset = nextInputIndex;
                while (!m_OutputReadIndex.compare_exchange_weak(output_offset,
                                                                nextInputIndex.getSameTagged(output_offset.getValue()),
                                                                std::memory_order::relaxed, std::memory_order::relaxed))
                    ;
            }

            return obj_emplaced;
        }

        void clean_memory() noexcept {
            constexpr auto MAX_IDX = std::numeric_limits<uint32_t>::max();
            auto const currentFollowIndex = m_OutputFollowIndex;
            auto const currentReadIndex = m_OutputReadIndex.load(std::memory_order::acquire).getValue();

            auto less_idx{MAX_IDX}, gequal_idx{MAX_IDX};
            for (auto const &reader_pos : std::span{m_ReaderPosPtrArray.data(), m_ReaderThreads}) {
                if (auto const output_pos = reader_pos->load(std::memory_order::acquire)) {
                    auto const output_index = static_cast<uint32_t>(output_pos - m_Array);
                    if (output_index >= currentFollowIndex) gequal_idx = std::min(gequal_idx, output_index);
                    else
                        less_idx = std::min(less_idx, output_index);
                }
            }

            m_OutputFollowIndex =
                    (gequal_idx != MAX_IDX) ? gequal_idx : ((less_idx != MAX_IDX) ? less_idx : currentReadIndex);
        }

    private:
        Ptr get_object() const noexcept {
            auto next_index = [lastElement = m_LastElementIndex](uint32_t index) {
                return index == lastElement ? 0 : index + 1;
            };

            auto output_index = m_OutputReadIndex.load(std::memory_order::relaxed);
            auto const input_index = m_InputIndex.load(std::memory_order::acquire);
            rb::detail::TaggedUint32 nextOutputIndex;

            do {
                if (input_index.getTag() < output_index.getTag() || input_index.getValue() == output_index.getValue())
                    return {};
                nextOutputIndex = input_index.getSameTagged(next_index(output_index.getValue()));
            } while (!m_OutputReadIndex.compare_exchange_weak(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                              std::memory_order::relaxed));
            return {m_Array + output_index.getValue(), m_Array + nextOutputIndex.getValue()};
        }

        Ptr get_object_check_once() const noexcept {
            auto output_index = m_OutputReadIndex.load(std::memory_order::relaxed);
            auto const input_index = m_InputIndex.load(std::memory_order::acquire);
            auto const currentIndex = output_index.getValue();

            if (input_index.getTag() < output_index.getTag() || input_index.getValue() == currentIndex) return {};

            auto const nextIndex = currentIndex == m_LastElementIndex ? 0 : (currentIndex + 1);
            auto const nextOutputIndex = input_index.getSameTagged(nextIndex);

            if (m_OutputReadIndex.compare_exchange_strong(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                          std::memory_order::relaxed))
                return {m_Array + currentIndex, m_Array + nextIndex};
            else
                return {};
        }

        void destroyAllObjects() noexcept {
            if constexpr (!std::is_trivially_destructible_v<ObjectType>) {
                auto const input_index = m_InputIndex.load(std::memory_order::relaxed).getValue();
                auto const output_index = m_OutputReadIndex.load(std::memory_order::relaxed).getValue();

                if (output_index == input_index) return;
                else if (output_index > input_index) {
                    std::destroy_n(m_Array + output_index, m_LastElementIndex - output_index + 1);
                    std::destroy_n(m_Array, input_index);
                } else
                    std::destroy_n(m_Array + output_index, input_index - output_index);
            }
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
        std::atomic<rb::detail::TaggedUint32> m_InputIndex{};
        uint32_t m_OutputFollowIndex{0};
        mutable std::atomic<rb::detail::TaggedUint32> m_OutputReadIndex{};

        uint32_t const m_LastElementIndex;
        uint16_t const m_ReaderThreads;
        ObjectType *const m_Array;
        ReaderPosPtrArray const m_ReaderPosPtrArray;
        allocator_type m_Allocator;
    };
}// namespace rb

#endif
