#ifndef OBJECTQUEUE_SCSP
#define OBJECTQUEUE_SCSP

#include "detail/rb_detail.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

template<typename ObjectType>
requires std::is_nothrow_destructible_v<ObjectType>
class ObjectQueue_SCSP {
public:
    class Ptr {
    public:
        Ptr() noexcept = default;

        Ptr(Ptr &&other) noexcept : object_queue{std::exchange(other.object_queue, nullptr)} {}

        Ptr(Ptr const &) = delete;
        Ptr &operator=(Ptr const &) = delete;

        Ptr &operator=(Ptr &&other) noexcept {
            if (object_queue) destroyObject();

            object_queue = std::exchange(other.object_queue, nullptr);
            return *this;
        }

        operator bool() const noexcept { return object_queue; }

        ObjectType &operator*() const noexcept {
            return object_queue->m_ObjectArray[object_queue->m_OutputIndex.load(std::memory_order::relaxed)];
        }

        ObjectType *get() const noexcept {
            return object_queue->m_ObjectArray + object_queue->m_OutputIndex.load(std::memory_order::relaxed);
        }

        ~Ptr() {
            if (object_queue) destroyObject();
        }

    private:
        friend class ObjectQueue_SCSP;

        explicit Ptr(ObjectQueue_SCSP const *object_queue) noexcept : object_queue{object_queue} {}

        void destroyObject() const noexcept {
            auto const output_index = object_queue->m_OutputIndex.load(std::memory_order::relaxed);
            object_queue->destroy(output_index);

            auto const nextIndex = output_index == object_queue->m_LastElementIndex ? 0 : (output_index + 1);
            object_queue->m_OutputIndex.store(nextIndex, std::memory_order::release);
        }

        ObjectQueue_SCSP const *object_queue{};
    };

public:
    ObjectQueue_SCSP(ObjectType *object_array, size_t array_size) noexcept
        : m_LastElementIndex{array_size - 1}, m_ObjectArray{object_array} {}

    ~ObjectQueue_SCSP() { destroyAllObjects(); }

    ObjectType *object_array() const noexcept { return m_ObjectArray; }

    uint32_t array_size() const noexcept { return m_LastElementIndex + 1; }

    bool empty() const noexcept {
        return m_InputIndex.load(std::memory_order::acquire) == m_OutputIndex.load(std::memory_order::acquire);
    }

    size_t size() const noexcept {
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
    }

    Ptr consume() const noexcept { return Ptr{this}; }

    template<typename Functor>
    requires std::is_nothrow_invocable_v<Functor, ObjectType &>
    decltype(auto) consume(Functor &&functor) const noexcept {
        auto const output_index = m_OutputIndex.load(std::memory_order::relaxed);

        rb::detail::ScopeGaurd const destroy_n_set_next_index{[this, output_index] {
            destroy(output_index);

            auto const nextIndex = output_index == m_LastElementIndex ? 0 : (output_index + 1);
            m_OutputIndex.store(nextIndex, std::memory_order::release);
        }};

        return std::forward<Functor>(functor)(m_ObjectArray[output_index]);
    }

    template<typename Functor>
    requires std::is_nothrow_invocable_v<Functor, ObjectType &> size_t consume_all(Functor &&functor)
    const noexcept {
        auto const input_index = m_InputIndex.load(std::memory_order::acquire);

        rb::detail::ScopeGaurd const set_next_index{
                [this, input_index] { m_OutputIndex.store(input_index, std::memory_order::release); }};

        auto consume_and_destroy = [this, &functor](uint32_t index, uint32_t end) {
            for (; index != end; ++index) {
                std::forward<Functor>(functor)(m_ObjectArray[index]);
                destroy(index);
            }
        };

        auto const output_index = m_OutputIndex.load(std::memory_order::relaxed);
        if (output_index > input_index) {
            auto const end1 = m_LastElementIndex + 1;
            auto const end2 = input_index;
            consume_and_destroy(output_index, end1);
            consume_and_destroy(0, end2);
            return (end1 - output_index) + end2;
        } else {
            consume_and_destroy(output_index, input_index);
            return input_index - output_index;
        }
    }

    bool push(ObjectType const &obj) noexcept { return emplace(obj); }

    bool push(ObjectType &&obj) noexcept { return emplace(std::move(obj)); }

    template<typename... Args>
    requires std::is_nothrow_constructible_v<ObjectType, Args...>
    bool emplace(Args &&...args) noexcept {
        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const output_index = m_OutputIndex.load(std::memory_order::acquire);
        auto const next_input_index = input_index == m_LastElementIndex ? 0 : (input_index + 1);

        if (next_input_index == output_index) return false;

        std::construct_at(m_ObjectArray + input_index, std::forward<Args>(args)...);

        m_InputIndex.store(next_input_index, std::memory_order::release);

        return true;
    }

    template<typename Functor>
    requires std::is_nothrow_invocable_r_v<size_t, Functor, ObjectType *, size_t>
            size_t emplace_back_n(Functor &&functor)
    noexcept {
        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const output_index = m_OutputIndex.load(std::memory_order::acquire);
        auto const count_avl = (input_index < output_index)
                                       ? (output_index - input_index - 1)
                                       : (m_LastElementIndex - input_index + static_cast<bool>(output_index));

        if (!count_avl) return 0;

        auto const obj_emplaced = std::forward<Functor>(functor)(m_ObjectArray + input_index, count_avl);
        auto const input_end = input_index + obj_emplaced;
        auto const next_input_index = (input_end == (m_LastElementIndex + 1)) ? 0 : input_end;

        m_InputIndex.store(next_input_index, std::memory_order::release);

        return obj_emplaced;
    }

private:
    void destroyAllObjects() noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) {
            auto const output_index = m_OutputIndex.load(std::memory_order::acquire);
            auto const input_index = m_InputIndex.load(std::memory_order::relaxed);

            if (output_index == input_index) return;
            else if (output_index > input_index) {
                std::destroy_n(m_ObjectArray + output_index, m_LastElementIndex - output_index + 1);
                std::destroy_n(m_ObjectArray, input_index);
            } else
                std::destroy_n(m_ObjectArray + output_index, input_index - output_index);
        }
    }

    void destroy(size_t index) const noexcept { std::destroy_at(m_ObjectArray + index); }

private:
    std::atomic<size_t> m_InputIndex{0};
    mutable std::atomic<size_t> m_OutputIndex{0};

    size_t const m_LastElementIndex;
    ObjectType *const m_ObjectArray;
};

#endif
