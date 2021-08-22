#ifndef OBJECTQUEUE_MCSP
#define OBJECTQUEUE_MCSP

#include "rb_detail.h"
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

template<typename ObjectType, bool isWriteProtected>
requires std::is_nothrow_destructible_v<ObjectType>
class ObjectQueue_MCSP {
public:
    class Ptr {
    public:
        Ptr() noexcept = default;

        Ptr(Ptr &&other) noexcept : object_ptr{std::exchange(other.object_ptr, nullptr)}, is_clean{other.is_clean} {}

        Ptr(Ptr const &) = delete;
        Ptr &operator=(Ptr const &) = delete;

        Ptr &operator=(Ptr &&other) noexcept {
            if (object_ptr) destroyObject();

            object_ptr = std::exchange(other.object_ptr, nullptr);
            is_clean = other.is_clean;
            return *this;
        }

        operator bool() const noexcept { return object_ptr; }

        ObjectType &operator*() const noexcept { return *object_ptr; }

        ObjectType *get() const noexcept { return object_ptr; }

        ~Ptr() {
            if (object_ptr) destroyObject();
        }

    private:
        friend class ObjectQueue_MCSP;

        explicit Ptr(ObjectType *object, std::atomic<bool> *is_clean) noexcept
            : object_ptr{object}, is_clean{is_clean} {}

        void destroyObject() const noexcept {
            std::destroy_at(object_ptr);
            is_clean->store(true, std::memory_order::release);
        }

        ObjectType *object_ptr{};
        std::atomic<bool> *is_clean;
    };

public:
    ObjectQueue_MCSP(ObjectType *object_array, std::atomic<bool> *clean_array, uint32_t array_size) noexcept
        : m_LastElementIndex{array_size - 1}, m_ObjectArray{object_array}, m_CleanArray{clean_array} {
        resetCleanArray();
    }

    ~ObjectQueue_MCSP() noexcept {
        if constexpr (isWriteProtected)
            while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                ;

        destroyAllObjects();
    }

    ObjectType *object_array() const noexcept { return m_ObjectArray; }

    std::atomic<bool> *clean_array() const noexcept { return m_CleanArray; }

    uint32_t array_size() const noexcept { return m_LastElementIndex + 1; }

    bool empty() const noexcept {
        return m_InputIndex.load(std::memory_order::acquire) == m_OutputHeadIndex.load(std::memory_order::acquire);
    }

    void clear() noexcept {
        if constexpr (isWriteProtected)
            while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                ;

        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        destroyAllObjects();
        resetCleanArray();

        m_InputIndex.store({}, std::memory_order::relaxed);
        m_OutputTailIndex = 0;
        m_OutputHeadIndex.store({}, std::memory_order::relaxed);
    }

    Ptr consume() const noexcept {
        auto const obj_index = get_index();
        return obj_index != INVALID_INDEX ? Ptr{m_ObjectArray + obj_index, m_CleanArray + obj_index} : Ptr{};
    }

    template<typename Functor>
    requires std::is_nothrow_invocable_v<Functor, ObjectType &>
    bool consume(Functor &&functor) const noexcept {
        auto const obj_index = get_index();
        if (obj_index != INVALID_INDEX) {
            std::forward<Functor>(functor)(m_ObjectArray[obj_index]);
            destroy(obj_index);
            return true;
        } else
            return false;
    }

    template<typename Functor>
    requires std::is_nothrow_invocable_v<Functor, ObjectType &> uint32_t consume_all(Functor &&functor)
    const noexcept {
        uint32_t consumed{0};
        while (true) {
            auto const obj_index = get_index();
            if (obj_index != INVALID_INDEX) {
                std::forward<Functor>(functor)(m_ObjectArray[obj_index]);
                destroy(obj_index);
                ++consumed;
            } else
                return consumed;
        }
    }

    bool push(ObjectType &obj) noexcept { return emplace(obj); }

    bool push(ObjectType &&obj) noexcept { return emplace(std::move(obj)); }

    template<typename... Args>
    requires std::is_nothrow_constructible_v<ObjectType, Args...>
    bool emplace(Args &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        cleanMemory();

        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const index_value = input_index.getValue();
        auto const output_index = m_OutputTailIndex;
        auto const nextInputIndexValue = index_value == m_LastElementIndex ? 0 : (index_value + 1);

        if (nextInputIndexValue == output_index) return false;

        std::construct_at(m_ObjectArray + index_value, std::forward<Args>(args)...);

        auto const nextInputIndex = input_index.getIncrTagged(nextInputIndexValue);
        m_InputIndex.store(nextInputIndex, std::memory_order::release);

        if (nextInputIndex.getTag() == 0) {
            auto output_offset = nextInputIndex;
            while (!m_OutputHeadIndex.compare_exchange_weak(output_offset,
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
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        cleanMemory();

        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const index_value = input_index.getValue();
        auto const output_index = m_OutputTailIndex;
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
            while (!m_OutputHeadIndex.compare_exchange_weak(output_offset,
                                                            nextInputIndex.getSameTagged(output_offset.getValue()),
                                                            std::memory_order::relaxed, std::memory_order::relaxed))
                ;
        }

        return obj_emplaced;
    }

private:
    uint32_t get_index() const noexcept {
        auto next_index = [lastElement = m_LastElementIndex](uint32_t index) {
            return index == lastElement ? 0 : index + 1;
        };

        auto const input_index = m_InputIndex.load(std::memory_order::acquire);
        auto output_index = m_OutputHeadIndex.load(std::memory_order::relaxed);
        rb_detail::TaggedUint32 nextOutputIndex;

        do {
            if (input_index.getTag() < output_index.getTag() || input_index.getValue() == output_index.getValue())
                return INVALID_INDEX;
            nextOutputIndex = input_index.getSameTagged(next_index(output_index.getValue()));
        } while (!m_OutputHeadIndex.compare_exchange_weak(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                          std::memory_order::relaxed));
        return output_index.getValue();
    }

    void cleanMemory() noexcept {
        auto clean_range = [clean_array = m_CleanArray](uint32_t index, uint32_t end) {
            for (; index != end; ++index)
                if (clean_array[index].load(std::memory_order::relaxed))
                    clean_array[index].store(false, std::memory_order::relaxed);
                else
                    break;
            return index;
        };

        auto const outputHeadIndex = m_OutputHeadIndex.load(std::memory_order::relaxed).getValue();
        if (m_OutputTailIndex == outputHeadIndex) return;
        else if (m_OutputTailIndex > outputHeadIndex) {
            m_OutputTailIndex = clean_range(m_OutputTailIndex, m_LastElementIndex + 1);
            if (m_OutputTailIndex != (m_LastElementIndex + 1)) return;
            else
                m_OutputTailIndex = 0;
        }

        m_OutputTailIndex = clean_range(m_OutputTailIndex, outputHeadIndex);
    }

    void destroyAllObjects() noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>)
            for (uint32_t index; (index = get_index()) != INVALID_INDEX;) std::destroy_at(m_ObjectArray + index);
    }

    void destroy(uint32_t index) const noexcept {
        std::destroy_at(m_ObjectArray + index);
        m_CleanArray[index].store(true, std::memory_order::release);
    }

    void resetCleanArray() noexcept {
        for (uint32_t i = 0; i <= m_LastElementIndex; ++i) m_CleanArray[i].store(false, std::memory_order::relaxed);
    }

private:
    static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();

    std::atomic<rb_detail::TaggedUint32> m_InputIndex{};
    uint32_t m_OutputTailIndex{0};
    mutable std::atomic<rb_detail::TaggedUint32> m_OutputHeadIndex{};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, rb_detail::Empty> m_WriteFlag{};

    uint32_t const m_LastElementIndex;
    ObjectType *const m_ObjectArray;
    std::atomic<bool> *const m_CleanArray;
};

#endif
