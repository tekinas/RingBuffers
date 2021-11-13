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

        using Index = detail::TaggedUint32;
        using ReaderPos = std::atomic<Index>;
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
                : m_ObjectPtr{std::exchange(other.m_ObjectPtr, nullptr)}, m_NextPos{other.m_NextPos} {}

            Ptr &operator=(Ptr &&other) noexcept {
                if (*this) destroy_object();
                m_ObjectPtr = std::exchange(other.m_ObjectPtr, nullptr);
                m_NextPos = other.m_NextPos;
                return *this;
            }

            Ptr(Ptr const &) = delete;

            Ptr &operator=(Ptr const &) = delete;

            ~Ptr() {
                if (*this) destroy_object();
            }

        private:
            Ptr(ObjectType *object_pos, Index next_pos) noexcept : m_ObjectPtr{object_pos}, m_NextPos{next_pos} {}

            void destroy_object() noexcept { std::destroy_at(std::exchange(m_ObjectPtr, nullptr)); }

            friend class ObjectQueue_MCSP;
            ObjectType *m_ObjectPtr{};
            Index m_NextPos;
        };

        class Reader {
        public:
            void release(Ptr &&p) const noexcept {
                detail::ScopeGaurd const rs = [&, next_pos{p.m_NextPos}] {
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

            ~Reader() { m_ReaderPos->store(Index::null(), std::memory_order::release); }

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
            clean_memory();
            destroyAllObjects();

            m_Allocator.deallocate_object(m_Array, m_LastElementIndex + 1);
            m_Allocator.deallocate_object(std::bit_cast<CacheLine *>(m_ReaderPosPtrArray[0]), m_ReaderThreads);
        }

        uint32_t array_size() const noexcept { return m_LastElementIndex; }

        bool empty() const noexcept {
            return m_InputIndex.load(std::memory_order::acquire).value() ==
                   m_OutputReadIndex.load(std::memory_order::acquire).value();
        }

        Reader get_reader(uint16_t thread_index) const noexcept {
            auto const reader_pos = m_ReaderPosPtrArray[thread_index];
            reader_pos->store(m_OutputReadIndex.load(std::memory_order::relaxed), std::memory_order::relaxed);

            std::atomic_thread_fence(std::memory_order::release);
            return Reader{this, reader_pos};
        }

        bool push(ObjectType const &obj) noexcept { return emplace(obj); }

        bool push(ObjectType &&obj) noexcept { return emplace(std::move(obj)); }

        template<typename... Args>
        requires std::is_nothrow_constructible_v<ObjectType, Args...>
        bool emplace(Args &&...args) noexcept {
            auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
            auto const index_value = input_index.value();
            auto const output_index = m_OutputFollowIndex.value();
            auto const nextInputIndexValue = index_value == m_LastElementIndex ? 0 : (index_value + 1);

            if (nextInputIndexValue == output_index) return false;

            std::construct_at(m_Array + index_value, std::forward<Args>(args)...);

            auto const nextInputIndex = input_index.incr_tagged(nextInputIndexValue);
            m_InputIndex.store(nextInputIndex, std::memory_order::release);

            if (nextInputIndex.tag() == 0) {
                auto output_offset = nextInputIndex;
                while (!m_OutputReadIndex.compare_exchange_weak(output_offset,
                                                                nextInputIndex.same_tagged(output_offset.value()),
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
            auto const index_value = input_index.value();
            auto const output_index = m_OutputFollowIndex.value();
            auto const count_avl = (index_value < output_index)
                                           ? (output_index - index_value - 1)
                                           : (m_LastElementIndex - index_value + static_cast<bool>(output_index));

            if (!count_avl) return 0;

            auto const obj_emplaced = std::forward<Functor>(functor)(m_Array + index_value, count_avl);
            auto const input_end = index_value + obj_emplaced;
            auto const nextInputIndexValue = (input_end == (m_LastElementIndex + 1)) ? 0 : input_end;
            auto const nextInputIndex = input_index.incr_tagged(nextInputIndexValue);

            m_InputIndex.store(nextInputIndex, std::memory_order::release);

            if (nextInputIndex.tag() == 0) {
                auto output_offset = nextInputIndex;
                while (!m_OutputReadIndex.compare_exchange_weak(output_offset,
                                                                nextInputIndex.getSameTagged(output_offset.value()),
                                                                std::memory_order::relaxed, std::memory_order::relaxed))
                    ;
            }

            return obj_emplaced;
        }

        void clean_memory() noexcept {
            auto const currentFollowOffset = m_OutputFollowIndex;
            auto const currentReadOffset = m_OutputReadIndex.load(std::memory_order::acquire);

            auto less_value = [](auto l, auto r) { return l.value() < r.value(); };

            constexpr auto max_val = Index::max();
            constexpr auto null_val = Index::null();
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

            m_OutputFollowIndex =
                    (gequal_idx != max_val) ? gequal_idx : ((less_idx != max_val) ? less_idx : currentReadOffset);
        }

    private:
        Ptr get_object() const noexcept {
            auto next_index = [lastElement = m_LastElementIndex](uint32_t index) {
                return index == lastElement ? 0 : index + 1;
            };

            auto output_index = m_OutputReadIndex.load(std::memory_order::relaxed);
            auto const input_index = m_InputIndex.load(std::memory_order::acquire);
            Index nextOutputIndex;

            do {
                if (input_index.tag() < output_index.tag() || input_index.value() == output_index.value()) return {};
                nextOutputIndex = input_index.same_tagged(next_index(output_index.value()));
            } while (!m_OutputReadIndex.compare_exchange_weak(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                              std::memory_order::relaxed));
            return {m_Array + output_index.value(), nextOutputIndex};
        }

        Ptr get_object_check_once() const noexcept {
            auto output_index = m_OutputReadIndex.load(std::memory_order::relaxed);
            auto const input_index = m_InputIndex.load(std::memory_order::acquire);
            auto const currentIndex = output_index.value();

            if (input_index.tag() < output_index.tag() || input_index.value() == currentIndex) return {};

            auto const nextIndex = currentIndex == m_LastElementIndex ? 0 : (currentIndex + 1);
            auto const nextOutputIndex = input_index.same_tagged(nextIndex);

            if (m_OutputReadIndex.compare_exchange_strong(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                          std::memory_order::relaxed))
                return {m_Array + currentIndex, nextOutputIndex};
            else
                return {};
        }

        void destroyAllObjects() noexcept {
            if constexpr (!std::is_trivially_destructible_v<ObjectType>) {
                auto const input_index = m_InputIndex.load(std::memory_order::relaxed).value();
                auto const output_index = m_OutputReadIndex.load(std::memory_order::relaxed).value();

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
                std::construct_at(reader_pos_ptr, Index::null());
                readerPosArray[t] = reader_pos_ptr;
            }
            std::atomic_thread_fence(std::memory_order::release);
            return readerPosArray;
        }

    private:
        std::atomic<Index> m_InputIndex{};
        Index m_OutputFollowIndex{};
        mutable std::atomic<Index> m_OutputReadIndex{};

        uint32_t const m_LastElementIndex;
        uint16_t const m_ReaderThreads;
        ObjectType *const m_Array;
        ReaderPosPtrArray const m_ReaderPosPtrArray;
        allocator_type m_Allocator;
    };
}// namespace rb

#endif
