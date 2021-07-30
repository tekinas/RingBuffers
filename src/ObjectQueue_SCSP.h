#ifndef OBJECTQUEUE_SCSP
#define OBJECTQUEUE_SCSP

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>

template<typename ObjectType, bool isReadProtected, bool isWriteProtected>
class ObjectQueue_SCSP {
public:
    class Ptr {
    public:
        Ptr(Ptr &&other) noexcept : object_queue{std::exchange(other.object_queue, nullptr)} {}

        Ptr(Ptr const &) = delete;

        Ptr &operator=(Ptr &&other) noexcept {
            std::destroy_at(this);
            object_queue = std::exchange(other.object_queue, nullptr);
            return *this;
        }

        ObjectType &operator*() const noexcept {
            return object_queue->m_Array[object_queue->m_OutputIndex.load(std::memory_order::relaxed)];
        }

        ObjectType *get() const noexcept {
            return object_queue->m_Array + object_queue->m_OutputIndex.load(std::memory_order::relaxed);
        }

        ~Ptr() noexcept {
            if (object_queue) {
                auto const output_index = object_queue->m_OutputIndex.load(std::memory_order::relaxed);
                object_queue->destroy(output_index);

                auto const nextIndex = output_index == object_queue->m_LastElementIndex ? 0 : (output_index + 1);

                object_queue->m_OutputIndex.store(nextIndex, std::memory_order::release);
                if constexpr (isReadProtected) object_queue->m_ReadFlag.clear(std::memory_order::release);
            }
        }

    private:
        friend class ObjectQueue_SCSP;

        explicit Ptr(ObjectQueue_SCSP const *object_queue) noexcept : object_queue{object_queue} {}

        ObjectQueue_SCSP const *object_queue;
    };

public:
    ObjectQueue_SCSP(ObjectType *buffer, size_t count)
        : m_LastElementIndex{static_cast<uint32_t>(count) - 1}, m_Array{buffer} {}

    ~ObjectQueue_SCSP() noexcept { destroyAllObjects(); }

    bool empty() const noexcept {
        return m_InputIndex.load(std::memory_order::acquire) == m_OutputIndex.load(std::memory_order::relaxed);
    }

    bool reserve() const noexcept {
        if constexpr (isReadProtected) {
            if (m_ReadFlag.test_and_set(std::memory_order::relaxed)) return false;
            if (empty()) {
                m_ReadFlag.clear(std::memory_order::relaxed);
                return false;
            } else
                return true;
        } else
            return !empty();
    }

    uint32_t size() const noexcept {
        auto const output_index = m_OutputIndex.load(std::memory_order::acquire);
        auto const input_index = m_InputIndex.load(std::memory_order::acquire);
        if (input_index >= output_index) return input_index - output_index;
        else
            return m_LastElementIndex - output_index + 1 + input_index;
    }

    void clear() noexcept {
        destroyAllObjects();

        m_InputIndex.store(0, std::memory_order::relaxed);
        m_OutputIndex.store(0, std::memory_order::relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::relaxed);
    }

    Ptr consume() const noexcept { return Ptr{this}; }

    template<typename F>
    decltype(auto) consume(F &&functor) const noexcept {
        auto const output_index = m_OutputIndex.load(std::memory_order::relaxed);
        auto &object = m_Array[output_index];

        auto cleanup = [&, output_index] {/// destroy object and set next output index
            destroy(output_index);

            auto const nextIndex = output_index == m_LastElementIndex ? 0 : (output_index + 1);
            m_OutputIndex.store(nextIndex, std::memory_order::release);

            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);
        };

        using ReturnType = decltype(std::forward<F>(functor)(std::declval<ObjectType &>()));
        if constexpr (std::is_void_v<ReturnType>) {
            std::forward<F>(functor)(object);
            cleanup();
        } else {
            auto &&result{std::forward<F>(functor)(object)};
            cleanup();
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename F>
    uint32_t consume_all(F &&functor) const noexcept {
        auto const output_index = m_OutputIndex.load(std::memory_order::relaxed);
        auto const input_index = m_InputIndex.load(std::memory_order::acquire);

        auto consume_and_destroy = [&](uint32_t index, uint32_t end_index) {
            for (; index != end_index; ++index) {
                std::forward<F>(functor)(m_Array[index]);
                destroy(index);
            }
        };

        uint32_t objects_consumed;
        if (output_index > input_index) {
            auto const end1 = m_LastElementIndex + 1;
            auto const end2 = input_index;

            consume_and_destroy(output_index, end1);
            consume_and_destroy(0, end2);

            objects_consumed = (end1 - output_index) + end2;
        } else {
            /* if (output_index < input_index), case of (output_index == input_index)
               is not handled as consume functions must be called
               on non-empty queue. */
            consume_and_destroy(output_index, input_index);
            objects_consumed = input_index - output_index;
        }

        m_OutputIndex.store(input_index, std::memory_order::release);

        return objects_consumed;
    }

    template<typename T>
    requires std::same_as<std::remove_cvref_t<T>, ObjectType>
    bool push_back(T &&obj) noexcept { return emplace_back(std::forward<T>(obj)); }

    template<typename... Args>
    bool emplace_back(Args &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const output_index = m_OutputIndex.load(std::memory_order::acquire);
        auto const next_input_index = input_index == m_LastElementIndex ? 0 : (input_index + 1);

        if (next_input_index == output_index) return false;

        std::construct_at(m_Array + input_index, std::forward<Args>(args)...);

        m_InputIndex.store(next_input_index, std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);

        return true;
    }

    template<typename Functor>
    uint32_t emplace_back_n(Functor &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const output_index = m_OutputIndex.load(std::memory_order::acquire);
        auto const count_avl = (input_index < output_index)
                                       ? (output_index - input_index - 1)
                                       : (m_LastElementIndex - input_index + static_cast<bool>(output_index));

        if (!count_avl) return 0;

        auto const count_emplaced = std::forward<Functor>(functor)(m_Array + input_index, count_avl);
        auto const input_end = input_index + count_emplaced;
        auto const next_input_index = (input_end == (m_LastElementIndex + 1)) ? 0 : input_end;

        m_InputIndex.store(next_input_index, std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);

        return count_emplaced;
    }

private:
    void destroyAllObjects() noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) {
            auto const output_index = m_OutputIndex.load(std::memory_order::acquire);
            auto const input_index = m_InputIndex.load(std::memory_order::relaxed);

            if (output_index == input_index) return;
            if (output_index > input_index) {
                std::destroy_n(m_Array + output_index, m_LastElementIndex - output_index + 1);
                std::destroy_n(m_Array, input_index);
            } else
                std::destroy_n(m_Array + output_index, input_index - output_index);
        }
    }

    void destroy(uint32_t index) const noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) std::destroy_at(m_Array + index);
    }

private:
    class Empty {
    public:
        template<typename... T>
        explicit Empty(T &&...) noexcept {}
    };

    std::atomic<uint32_t> m_InputIndex{0};
    mutable std::atomic<uint32_t> m_OutputIndex{0};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Empty> m_WriteFlag{};
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, Empty> m_ReadFlag{};

    uint32_t const m_LastElementIndex;
    ObjectType *const m_Array;
};

#endif
