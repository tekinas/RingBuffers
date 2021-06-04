#ifndef FUNCTIONQUEUE_OBJECTQUEUE_SCSP_H
#define FUNCTIONQUEUE_OBJECTQUEUE_SCSP_H

#include <atomic>
#include <cstdint>
#include <memory>

template<typename ObjectType, bool isReadProtected, bool isWriteProtected>
class ObjectQueue_SCSP {
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
        friend class ObjectQueue;

        explicit Ptr(ObjectType const *object_queue) noexcept: object_queue{object_queue} {}

        ObjectQueue_SCSP const *object_queue;
    };


public:
    inline ObjectQueue_SCSP(void *buffer, uint32_t count) : m_Array{static_cast<ObjectType *>(buffer)},
                                                            m_LastElementIndex{count - 1} {
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
    }

    inline ~ObjectQueue_SCSP() noexcept {
        destroyAllObjects();
    }

    inline auto size() const noexcept { return m_Remaining.load(std::memory_order_relaxed); }

    inline void clear() noexcept {
        destroyAllObjects();

        m_InputIndex = 0;
        m_Remaining.store(0, std::memory_order_relaxed);
        m_OutPutIndex.store(0, std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
    }

    inline bool reserve() const noexcept {
        if constexpr (isReadProtected) {
            if (m_ReadFlag.test_and_set(std::memory_order_relaxed)) return false;
            if (!m_Remaining.load(std::memory_order_acquire)) {
                m_ReadFlag.clear(std::memory_order_relaxed);
                return false;
            } else return true;
        } else return m_Remaining.load(std::memory_order_acquire);
    }

    template<typename F>
    decltype(auto) consume(F &&functor) const noexcept {
        auto const output_index = m_OutPutIndex.load(std::memory_order_relaxed);

        auto cleanup = [&, output_index] { /// destroy obj, decrement remaining and set next output index
            destroy(output_index);
            m_Remaining.fetch_sub(1, std::memory_order_relaxed);

            auto const nextIndex = output_index == m_LastElementIndex ? 0 : (output_index + 1);
            if constexpr (isReadProtected) {
                m_OutPutIndex.store(nextIndex, std::memory_order_relaxed);
                m_ReadFlag.clear(std::memory_order_release);
            } else
                m_OutPutIndex.store(nextIndex, std::memory_order_release);
        };

        using ReturnType = decltype(std::forward<F>(functor)(std::declval<ObjectType &>()));
        if constexpr (std::is_same_v<void, ReturnType>) {
            std::forward<F>(functor)(m_Array[output_index]);
            cleanup();
        } else {
            auto &&result{std::forward<F>(functor)(m_Array[output_index])};
            cleanup();
            return std::forward<decltype(result)>(result);
        }
    }

    inline Ptr consume() const noexcept {
        return Ptr{this};
    }

    template<typename F>
    uint32_t consume_all(F &&functor) const noexcept {
        auto const rem = m_Remaining.load(std::memory_order_relaxed);
        auto output_index = m_OutPutIndex.load(std::memory_order_relaxed);

        if ((m_LastElementIndex - output_index + 1) >= rem) {
            auto count = rem;
            --output_index;
            while (count--) {
                ++output_index;
                std::forward<F>(functor)(m_Array[output_index]);
                destroy(output_index);
            }

            output_index = output_index == m_LastElementIndex ? 0 : (output_index + 1); /// set next index
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

        m_Remaining.fetch_sub(rem, std::memory_order_relaxed);

        if constexpr (isReadProtected) {
            m_OutPutIndex.store(output_index, std::memory_order_relaxed);
            m_ReadFlag.clear(std::memory_order_release);
        } else
            m_OutPutIndex.store(output_index, std::memory_order_release);

        return rem;
    }

    template<typename T>
    requires std::same_as<std::decay_t<T>, ObjectType>
    bool push_back(T &&obj) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        if (m_OutPutIndex.load(std::memory_order_acquire) == m_InputIndex &&
            m_Remaining.load(std::memory_order_relaxed))
            return false;

        std::construct_at(m_Array + m_InputIndex, std::forward<T>(obj));

        m_Remaining.fetch_add(1, std::memory_order_release);

        m_InputIndex = m_InputIndex == m_LastElementIndex ? 0 : (m_InputIndex + 1);

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

        if (m_OutPutIndex.load(std::memory_order_acquire) == m_InputIndex &&
            m_Remaining.load(std::memory_order_relaxed))
            return false;

        std::construct_at(m_Array + m_InputIndex, std::forward<Args>(args)...);

        m_Remaining.fetch_add(1, std::memory_order_release);

        m_InputIndex = m_InputIndex == m_LastElementIndex ? 0 : (m_InputIndex + 1);

        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

private:
    inline void destroyAllObjects() noexcept {
        if constexpr(!std::is_trivially_destructible_v<ObjectType>) {
            auto const output_index = m_OutPutIndex.load(std::memory_order_relaxed);
            if (output_index > m_InputIndex ||
                (output_index == m_InputIndex && m_Remaining.load(std::memory_order_relaxed))) {
                std::destroy_n(m_Array + output_index, m_LastElementIndex - output_index + 1);
                std::destroy_n(m_Array, output_index);
            } else if (output_index < m_InputIndex)
                std::destroy_n(m_Array + output_index, m_InputIndex - output_index);
        }
    }

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

    uint32_t m_InputIndex{0};
    mutable std::atomic<uint32_t> m_Remaining{0};
    mutable std::atomic<uint32_t> m_OutPutIndex{0};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, Null> m_ReadFlag;

    uint32_t const m_LastElementIndex;
    ObjectType *const m_Array;
};


#endif //FUNCTIONQUEUE_OBJECTQUEUE_SCSP_H
