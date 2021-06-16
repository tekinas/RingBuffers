#ifndef FUNCTIONQUEUE_OjectQueue_MCSP_H
#define FUNCTIONQUEUE_OjectQueue_MCSP_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <type_traits>

template<typename T>
concept ObjectQueueFreeable = requires(T *obj) {
    /// used by the writer thread to know if the given memory can be reused
    { OQ_IsObjectFree(obj) } -> std::same_as<bool>;

    /// used by the reader threads to 'mark' the memory as freed after the object in it is destroyed
    { OQ_FreeObject(obj) } -> std::same_as<void>;
};

template<typename ObjectType, bool isReadProtected, bool isWriteProtected>
requires ObjectQueueFreeable<ObjectType>

class OjectQueue_MCSP {
public:
    class Ptr {
    public:
        inline Ptr(Ptr &&other) noexcept : object_ptr{std::exchange(other.object_ptr, nullptr)} {}

        Ptr(Ptr const &) = delete;

        inline Ptr &operator=(Ptr &&other) noexcept {
            this->~Ptr();
            object_ptr = std::exchange(other.object_ptr, nullptr);
            return *this;
        }

        inline operator bool() const noexcept { return object_ptr; }

        inline ObjectType &operator*() const noexcept { return *object_ptr; }

        inline ObjectType *get() const noexcept { return object_ptr; }

        inline ~Ptr() noexcept {
            if (object_ptr) {
                std::destroy_at(object_ptr);
                OQ_FreeObject(object_ptr);/// atomically reset object's memory to free status
            }
        }

    private:
        friend class OjectQueue_MCSP;

        explicit Ptr(ObjectType *object) noexcept : object_ptr{object} {}

        ObjectType *object_ptr;
    };

public:
    inline OjectQueue_MCSP(ObjectType *buffer, uint32_t count) : m_Array{buffer}, m_LastElementIndex{count - 1} {
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
    }

    ~OjectQueue_MCSP() noexcept { destroyAllObjects(); }

    inline auto size() const noexcept {
        auto const output_index = m_OutputHeadIndex.load(std::memory_order_relaxed);
        auto const input_index = m_InputIndex.load(std::memory_order_acquire);
        if (input_index >= output_index) return input_index - output_index;
        else
            return m_LastElementIndex - output_index + 1 + input_index;
    }

    inline bool empty() const noexcept {
        return m_InputIndex.load(std::memory_order_acquire) == m_OutputHeadIndex.load(std::memory_order_relaxed);
    }

    template<typename T>
    requires std::same_as<std::decay_t<T>, ObjectType>
    bool push_back(T &&obj) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order_relaxed);
        auto const output_index = cleanMemory();
        auto const next_input_index = input_index == m_LastElementIndex ? 0 : (input_index + 1);

        if (next_input_index == output_index) return false;

        std::construct_at(m_Array + input_index, std::forward<T>(obj));

        m_InputIndex.store(next_input_index, std::memory_order_release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }

        return true;
    }

    template<typename... Args>
    bool emplace_back(Args &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order_relaxed);
        auto const output_index = cleanMemory();
        auto const next_input_index = input_index == m_LastElementIndex ? 0 : (input_index + 1);

        if (next_input_index == output_index) return false;

        std::construct_at(m_Array + input_index, std::forward<Args>(args)...);

        m_InputIndex.store(next_input_index, std::memory_order_release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }

        return true;
    }

    template<typename Functor>
    uint32_t emplace_back_n(Functor &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order_relaxed);
        auto const output_index = cleanMemory();
        auto const count_avl = (input_index < output_index)
                                       ? (output_index - input_index - 1)
                                       : (m_LastElementIndex - input_index + static_cast<bool>(output_index));

        if (!count_avl) return 0;

        auto const count_emplaced = std::forward<Functor>(functor)(m_Array + input_index, count_avl);
        auto const input_end = input_index + count_emplaced;
        auto const next_input_index = (input_end == (m_LastElementIndex + 1)) ? 0 : input_end;

        m_InputIndex.store(next_input_index, std::memory_order_release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }

        return count_emplaced;
    }

private:
    inline uint32_t cleanMemory() noexcept {/// this is the only function that modifies m_OutputTailIndex
        auto const output_tail = m_OutputTailIndex.load(std::memory_order_relaxed);
        auto const output_head = m_OutputHeadIndex.load(std::memory_order_acquire);

        auto checkAndForwardIndex = [this](uint32_t tail, uint32_t end) {
            for (; tail != end && OQ_IsObjectFree(m_Array + tail); ++tail)
                ;
            return tail;
        };

        if (output_tail == output_head) return output_tail;
        else if (output_tail < output_head) {
            auto const new_tail = checkAndForwardIndex(output_tail, output_head);
            if (new_tail != output_tail) m_OutputTailIndex.store(new_tail, std::memory_order_relaxed);

            return new_tail;
        } else {
            auto const array_end = m_LastElementIndex + 1;
            auto new_tail = checkAndForwardIndex(output_tail, array_end);
            if (new_tail == array_end) { new_tail = checkAndForwardIndex(0, output_head); }
            m_OutputTailIndex.store(new_tail, std::memory_order_relaxed);

            return new_tail;
        }
    }

    void destroyAllObjects() noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) {
            auto const input_index = m_InputIndex.load(std::memory_order_relaxed);
            auto const output_tail = m_OutputTailIndex.load(std::memory_order_relaxed);
            auto const output_head = m_OutputHeadIndex.load(std::memory_order_acquire);

            auto check_and_destroy_range = [this](uint32_t start, uint32_t end) {
                for (; start != end; ++start)
                    if (auto obj_ptr = m_Array + start; !OQ_IsObjectFree(obj_ptr)) std::destroy_at(obj_ptr);
            };

            auto destroy_range = [this](uint32_t start, uint32_t end) {
                for (; start != end; ++start) std::destroy_at(m_Array + start);
            };

            if (output_tail > output_head) {
                check_and_destroy_range(output_tail, m_LastElementIndex + 1);
                check_and_destroy_range(0, output_head);
            } else if (output_tail < output_head)
                check_and_destroy_range(output_tail, output_head);

            if (output_head == input_index) return;
            if (output_head > input_index) {
                destroy_range(output_head, m_LastElementIndex + 1);
                destroy_range(0, input_index);
            } else
                destroy_range(output_head, input_index);
        }
    }

    inline void destroy(uint32_t index) const noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) { std::destroy_at(m_Array + index); }
        OQ_FreeObject(m_Array + index);
    }

private:
    class Null {
    public:
        template<typename... T>
        explicit Null(T &&...) noexcept {}
    };

    std::atomic<uint32_t> m_InputIndex{0};
    std::atomic<uint32_t> m_OutputTailIndex{0};
    mutable std::atomic<uint32_t> m_OutputHeadIndex{0};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;

    uint32_t const m_LastElementIndex;
    ObjectType *const m_Array;
};

#endif
