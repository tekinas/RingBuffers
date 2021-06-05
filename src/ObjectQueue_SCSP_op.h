#ifndef FUNCTIONQUEUE_ObjectQueue_SCSP_OP_H
#define FUNCTIONQUEUE_ObjectQueue_SCSP_OP_H

#include <atomic>
#include <cstdint>
#include <memory>

template<typename ObjectType, bool isReadProtected, bool isWriteProtected>
class ObjectQueue_SCSP_op {
public:

    class Ptr {
    public:
        inline Ptr(Ptr &&other) noexcept: object_queue{std::exchange(other.object_queue, nullptr)} {}

        Ptr(Ptr const &) = delete;

        inline Ptr &operator=(Ptr &&other) noexcept {
            this->~Ptr();
            object_queue = std::exchange(other.object_queue, nullptr);
            return *this;
        }

        inline ObjectType &operator*() const noexcept {
            return object_queue->m_Array[object_queue->m_OutPutIndex.load(std::memory_order_relaxed)];
        }

        inline ObjectType *get() const noexcept {
            return object_queue->m_Array + object_queue->m_OutPutIndex.load(std::memory_order_relaxed);
        }

        inline ~Ptr() noexcept {
            if (object_queue) {
                auto const output_index = object_queue->m_OutPutIndex.load(std::memory_order_relaxed);
                object_queue->destroy(output_index);
                object_queue->m_Remaining.fetch_sub(1, std::memory_order_relaxed);

                auto const nextIndex = output_index == object_queue->m_LastElementIndex ? 0 : (output_index + 1);
                if constexpr (isReadProtected) {
                    object_queue->m_OutPutIndex.store(nextIndex, std::memory_order_relaxed);
                    object_queue->m_ReadFlag.clear(std::memory_order_release);
                } else
                    object_queue->m_OutPutIndex.store(nextIndex, std::memory_order_release);
            }
        }

    private:
        friend class ObjectQueue_SCSP_op;

        explicit Ptr(ObjectType const *object_queue) noexcept: object_queue{object_queue} {}

        ObjectQueue_SCSP_op const *object_queue;
    };


public:
    inline ObjectQueue_SCSP_op(void *buffer, uint32_t count) : m_Array{static_cast<ObjectType *>(buffer)},
                                                               m_LastElementIndex{count - 1} {
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
    }

    inline ~ObjectQueue_SCSP_op() noexcept {
//        destroyAllObjects();
    }

    inline auto size() const noexcept {
        auto const output_index = m_OutPutIndex.load(std::memory_order_relaxed);
        auto const input_index = m_InputIndex.load(std::memory_order_acquire);
        if (input_index >= output_index) return input_index - output_index;
        else return m_LastElementIndex - output_index + input_index + 1;
    }

    inline void clear() noexcept {
//        destroyAllObjects();

        m_InputIndex.store(0, std::memory_order_relaxed);
        m_OutPutIndex.store(0, std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
    }

    bool empty() const noexcept {
        return m_InputIndex.load(std::memory_order_acquire) == m_OutPutIndex.load(std::memory_order_relaxed);
    }

    template<typename F>
    decltype(auto) consume(F &&functor) const noexcept {
        auto const output_index = m_OutPutIndex.load(std::memory_order_relaxed);
        auto &obj = m_Array[output_index];

        auto cleanup = [&, output_index] { /// destroy obj, decrement remaining and set next output index
            obj.~ObjectType();

            auto const nextIndex = output_index == m_LastElementIndex ? 0 : (output_index + 1);
            if constexpr (isReadProtected) {
                m_OutPutIndex.store(nextIndex, std::memory_order_relaxed);
                m_ReadFlag.clear(std::memory_order_release);
            } else
                m_OutPutIndex.store(nextIndex, std::memory_order_release);
        };

        using ReturnType = decltype(std::forward<F>(functor)(std::declval<ObjectType &>()));
        if constexpr (std::is_same_v<void, ReturnType>) {
            std::forward<F>(functor)(obj);
            cleanup();
        } else {
            auto &&result{std::forward<F>(functor)(obj)};
            cleanup();
            return std::forward<decltype(result)>(result);
        }
    }

    inline Ptr consume() const noexcept {
        return Ptr{this};
    }

    /*template<typename F>
    uint32_t consume_all(F &&functor) const noexcept {
        auto output_index = m_OutPutIndex.load(std::memory_order_relaxed);
        auto const rem = size();

        if ((m_LastElementIndex - output_index + 1) >= rem) {
            auto const end = output_index + rem;
            while (output_index != end) {
                std::forward<F>(functor)(m_Array[output_index]);
                destroy(output_index);
                ++output_index;
            }

            if (output_index == m_LastElementIndex + 1) output_index = 0;
        } else {
            auto const end1 = m_LastElementIndex + 1;
            auto const end2 = rem - (end1 - output_index);

            while (output_index != end1) {
                std::forward<F>(functor)(m_Array[output_index]);
                destroy(output_index);
                ++output_index;
            }

            output_index = 0;
            while (output_index != end2) {
                std::forward<F>(functor)(m_Array[output_index]);
                destroy(output_index);
                ++output_index;
            }
        }

        if constexpr (isReadProtected) {
            m_OutPutIndex.store(output_index, std::memory_order_relaxed);
            m_ReadFlag.clear(std::memory_order_release);
        } else
            m_OutPutIndex.store(output_index, std::memory_order_release);

        return rem;
    }*/

    template<typename F>
    uint32_t consume_all(F &&functor) const noexcept {
        const size_t read_index = m_OutPutIndex.load(std::memory_order_relaxed); // only written from pop thread
        const size_t max_size = m_LastElementIndex + 1;
        const size_t output_count = size();

        auto run_functor_and_delete = [&](auto first, auto last) {
            for (; first != last; ++first) {
                std::forward<F>(functor)(*first);
                first->~ObjectType();
            }
        };

        size_t new_read_index = read_index + output_count;

        if (read_index + output_count > max_size) {
            /* copy data in two sections */
            const size_t count0 = max_size - read_index;
            const size_t count1 = output_count - count0;

            run_functor_and_delete(m_Array + read_index, m_Array + max_size);
            run_functor_and_delete(m_Array, m_Array + count1);

            new_read_index -= max_size;
        } else {
            run_functor_and_delete(m_Array + read_index, m_Array + read_index + output_count);

            if (new_read_index == max_size)
                new_read_index = 0;
        }

        m_OutPutIndex.store(new_read_index, std::memory_order_release);
        return output_count;
    }

    template<typename T>
    requires std::same_as<std::decay_t<T>, ObjectType>
    bool push_back(T &&obj) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order_relaxed);
        auto const output_index = m_OutPutIndex.load(std::memory_order_acquire);
        auto const next_input_index = m_InputIndex == m_LastElementIndex ? 0 : (m_InputIndex + 1);

        if (next_input_index == output_index) return false;

        std::construct_at(m_Array + input_index, std::forward<T>(obj));

        m_InputIndex.store(next_input_index, std::memory_order_release);

        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

    template<typename... Args>
    bool emplace_back(Args &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order_relaxed);
        auto const output_index = m_OutPutIndex.load(std::memory_order_acquire);
        auto const next_input_index = m_InputIndex == m_LastElementIndex ? 0 : (m_InputIndex + 1);

        if (next_input_index == output_index) return false;

        std::construct_at(m_Array + input_index, std::forward<Args>(args)...);

        m_InputIndex.store(next_input_index, std::memory_order_release);

        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

private:
    /*inline void destroyAllObjects() noexcept {
        if constexpr(!std::is_trivially_destructible_v<ObjectType>) {
            auto const output_index = m_OutPutIndex.load(std::memory_order_relaxed);
            if (output_index > m_InputIndex ||
                (output_index == m_InputIndex && m_Remaining.load(std::memory_order_relaxed))) {
                std::destroy_n(m_Array + output_index, m_LastElementIndex - output_index + 1);
                std::destroy_n(m_Array, output_index);
            } else if (output_index < m_InputIndex)
                std::destroy_n(m_Array + output_index, m_InputIndex - output_index);
        }
    }*/

    inline void destroy(uint32_t index) const noexcept {
        if constexpr(!std::is_trivially_destructible_v<ObjectType>) {
            std::destroy_at(m_Array + index);
        }
    }

private:
    class Null {
    public:
        template<typename ...T>
        explicit Null(T &&...) noexcept {}
    };

    std::atomic<uint32_t> m_InputIndex{0};
    mutable std::atomic<uint32_t> m_OutPutIndex{0};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, Null> m_ReadFlag;

    uint32_t const m_LastElementIndex;
    ObjectType *const m_Array;
};

#endif
