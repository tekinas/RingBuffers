#ifndef OBJECTQUEUE_MCSP
#define OBJECTQUEUE_MCSP

#include "detail/rb_detail.h"
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

template<typename ObjectType, size_t max_reader_threads = 8>
requires std::is_nothrow_destructible_v<ObjectType>
class ObjectQueue_MCSP {
private:
    using ObjectPtr = std::array<ObjectType *, 2>;
    using ReaderPos = std::atomic<ObjectType *>;
    using ReaderPosPtrArray = std::array<ReaderPos *, max_reader_threads>;

public:
    class Ptr {
    public:
        Ptr() noexcept = default;

        Ptr(Ptr &&other) noexcept
            : object_pos{std::exchange(other.object_pos, {nullptr, nullptr})}, reader_pos{other.reader_pos} {}

        Ptr(Ptr const &) = delete;

        Ptr &operator=(Ptr const &) = delete;

        Ptr &operator=(Ptr &&other) noexcept {
            if (*this) destroyObject();

            object_pos = std::exchange(other.object_pos, {nullptr, nullptr});
            reader_pos = other.reader_pos;
            return *this;
        }

        operator bool() const noexcept { return object_pos[0]; }

        ObjectType &operator*() const noexcept { return *object_pos[0]; }

        ObjectType *get() const noexcept { return object_pos[0]; }

        ~Ptr() {
            if (*this) destroyObject();
        }

    private:
        friend class ObjectQueue_MCSP;

        Ptr(ObjectPtr object_pos, ReaderPos *reader_pos) noexcept : object_pos{object_pos}, reader_pos{reader_pos} {}

        void destroyObject() const noexcept {
            std::destroy_at(object_pos[0]);
            reader_pos->store(object_pos[1], std::memory_order::release);
        }

        ObjectPtr object_pos{nullptr, nullptr};
        ReaderPos *reader_pos;
    };

    class Reader {
    public:
        auto consume() const noexcept { return Ptr{m_ObjectQueue->get_object(), m_ReaderPos}; }

        auto consume(rb::check_once_tag) const noexcept {
            return Ptr{m_ObjectQueue->get_object_check_once(), m_ReaderPos};
        }

        template<typename Functor>
        requires std::is_nothrow_invocable_v<Functor, ObjectType &>
        auto consume(Functor &&functor) const noexcept {
            return consume_impl<&ObjectQueue_MCSP::get_object>(std::forward<Functor>(functor));
        }

        template<typename Functor>
        requires std::is_nothrow_invocable_v<Functor, ObjectType &>
        auto consume(rb::check_once_tag, Functor &&functor) const noexcept {
            return consume_impl<&ObjectQueue_MCSP::get_object_check_once>(std::forward<Functor>(functor));
        }

        template<typename Functor>
        requires std::is_nothrow_invocable_v<Functor, ObjectType &>
        auto consume_all(Functor &&functor) const noexcept {
            return consume_all_impl<&ObjectQueue_MCSP::get_object>(std::forward<Functor>(functor));
        }

        template<typename Functor>
        requires std::is_nothrow_invocable_v<Functor, ObjectType &>
        auto consume_all(rb::check_once_tag, Functor &&functor) const noexcept {
            return consume_all_impl<&ObjectQueue_MCSP::get_object_check_once>(std::forward<Functor>(functor));
        }

        ~Reader() { m_ReaderPos->store(nullptr, std::memory_order::release); }

    private:
        template<auto get_object, typename Functor>
        bool consume_impl(Functor &&functor) const noexcept {
            if (auto const object_pos = (m_ObjectQueue->*get_object)(); object_pos[0]) {
                std::forward<Functor>(functor)(*object_pos[0]);
                std::destroy_at(object_pos[0]);
                m_ReaderPos->store(object_pos[1], std::memory_order::release);
                return true;
            } else
                return false;
        }

        template<auto get_object, typename Functor>
        uint32_t consume_all_impl(Functor &&functor) const noexcept {
            uint32_t consumed{0};
            ObjectType *last_free_pos;
            while (true) {
                if (auto const object_pos = (m_ObjectQueue->*get_object)(); object_pos[0]) {
                    std::forward<Functor>(functor)(*object_pos[0]);
                    std::destroy_at(object_pos[0]);
                    ++consumed;
                    last_free_pos = object_pos[1];
                } else {
                    if (consumed) m_ReaderPos->store(last_free_pos, std::memory_order::release);
                    return consumed;
                }
            }
        }

        Reader(ObjectQueue_MCSP const *objectQueue, ReaderPos *reader_pos) noexcept
            : m_ObjectQueue{objectQueue}, m_ReaderPos{reader_pos} {}

    private:
        friend class ObjectQueue_MCSP;

        ObjectQueue_MCSP const *const m_ObjectQueue{};
        ReaderPos *const m_ReaderPos;
    };

    using allocator_type = std::pmr::polymorphic_allocator<>;

    ObjectQueue_MCSP(uint32_t array_size, uint16_t reader_threads, allocator_type allocator = {}) noexcept
        : m_LastElementIndex{array_size}, m_ReaderThreads{reader_threads},
          m_ObjectArray{allocator.allocate_object<ObjectType>(array_size + 1)},
          m_ReaderPosArray{getReaderPosArray(allocator, reader_threads)}, m_Allocator{allocator} {
        resetReaderPosArray();
    }

    ~ObjectQueue_MCSP() {
        destroyAllObjects();

        m_Allocator.deallocate_object(m_ObjectArray, m_LastElementIndex + 1);
        for (uint16_t t = 0; t != m_ReaderThreads; ++t)
            m_Allocator.deallocate_bytes(m_ReaderPosArray[t], 64, alignof(ReaderPos));
    }

    uint32_t array_size() const noexcept { return m_LastElementIndex; }

    bool empty() const noexcept {
        return m_InputIndex.load(std::memory_order::acquire).getValue() ==
               m_OutputReadIndex.load(std::memory_order::acquire).getValue();
    }

    void clear() noexcept {
        destroyAllObjects();

        m_InputIndex.store({}, std::memory_order::relaxed);
        m_OutputFollowIndex = 0;
        m_OutputReadIndex.store({}, std::memory_order::relaxed);
        resetReaderPosArray();
    }

    Reader getReader(uint16_t thread_index) const noexcept {
        auto const reader_pos = m_ReaderPosArray[thread_index];
        reader_pos->store(m_ObjectArray + m_OutputReadIndex.load(std::memory_order::relaxed).getValue(),
                          std::memory_order::release);
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

        std::construct_at(m_ObjectArray + index_value, std::forward<Args>(args)...);

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

        auto const obj_emplaced = std::forward<Functor>(functor)(m_ObjectArray + index_value, count_avl);
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
        auto get_distance = [follow_index = m_OutputFollowIndex,
                             rem = m_LastElementIndex - m_OutputFollowIndex](uint32_t index) {
            return (index >= follow_index) ? (index - follow_index) : (rem + index);
        };

        auto const currentReadIndex = m_OutputReadIndex.load(std::memory_order::acquire).getValue();
        constexpr auto MAX_DIST = std::numeric_limits<uint32_t>::max();

        auto min_dist = MAX_DIST;
        uint32_t nearest_index;

        for (uint16_t t = 0; t != m_ReaderThreads; ++t) {
            if (auto const output_pos = m_ReaderPosArray[t]->load(std::memory_order::acquire)) {
                auto const output_index = output_pos - m_ObjectArray;
                auto const dist = get_distance(output_index);
                if (dist < min_dist) {
                    nearest_index = output_index;
                    min_dist = dist;
                }
            }
        }
        m_OutputFollowIndex = min_dist != MAX_DIST ? nearest_index : currentReadIndex;
    }

private:
    ObjectPtr get_object() const noexcept {
        auto next_index = [lastElement = m_LastElementIndex](uint32_t index) {
            return index == lastElement ? 0 : index + 1;
        };

        auto output_index = m_OutputReadIndex.load(std::memory_order::relaxed);
        auto const input_index = m_InputIndex.load(std::memory_order::acquire);
        rb::detail::TaggedUint32 nextOutputIndex;

        do {
            if (input_index.getTag() < output_index.getTag() || input_index.getValue() == output_index.getValue())
                return {nullptr, nullptr};
            nextOutputIndex = input_index.getSameTagged(next_index(output_index.getValue()));
        } while (!m_OutputReadIndex.compare_exchange_weak(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                          std::memory_order::relaxed));
        return {m_ObjectArray + output_index.getValue(), m_ObjectArray + nextOutputIndex.getValue()};
    }

    ObjectPtr get_object_check_once() const noexcept {
        auto output_index = m_OutputReadIndex.load(std::memory_order::relaxed);
        auto const input_index = m_InputIndex.load(std::memory_order::acquire);
        auto const currentIndex = output_index.getValue();

        if (input_index.getTag() < output_index.getTag() || input_index.getValue() == currentIndex)
            return {nullptr, nullptr};

        auto const nextIndex = currentIndex == m_LastElementIndex ? 0 : (currentIndex + 1);
        auto const nextOutputIndex = input_index.getSameTagged(nextIndex);

        if (m_OutputReadIndex.compare_exchange_strong(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                      std::memory_order::relaxed))
            return {m_ObjectArray + currentIndex, m_ObjectArray + nextIndex};
        else
            return {nullptr, nullptr};
    }

    void destroyAllObjects() noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) {
            auto const input_index = m_InputIndex.load(std::memory_order::relaxed).getValue();
            auto const output_index = m_OutputReadIndex.load(std::memory_order::relaxed).getValue();

            if (output_index == input_index) return;
            else if (output_index > input_index) {
                std::destroy_n(m_ObjectArray + output_index, m_LastElementIndex - output_index + 1);
                std::destroy_n(m_ObjectArray, input_index);
            } else
                std::destroy_n(m_ObjectArray + output_index, input_index - output_index);
        }
    }

    void resetReaderPosArray() noexcept {
        for (uint16_t t = 0; t != m_ReaderThreads; ++t) m_ReaderPosArray[t]->store(nullptr, std::memory_order::relaxed);
    }

    static auto getReaderPosArray(allocator_type allocator, uint16_t threads) noexcept {
        ReaderPosPtrArray readerPosArray;
        for (uint16_t t = 0; t != threads; ++t)
            readerPosArray[t] = static_cast<ReaderPos *>(allocator.allocate_bytes(64, alignof(ReaderPos)));
        return readerPosArray;
    }

private:
    std::atomic<rb::detail::TaggedUint32> m_InputIndex{};
    uint32_t m_OutputFollowIndex{0};
    mutable std::atomic<rb::detail::TaggedUint32> m_OutputReadIndex{};

    uint32_t const m_LastElementIndex;
    uint16_t const m_ReaderThreads;
    ObjectType *const m_ObjectArray;
    ReaderPosPtrArray const m_ReaderPosArray;
    allocator_type m_Allocator;
};

#endif
