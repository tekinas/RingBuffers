#ifndef OBJECTQUEUE_MCSP
#define OBJECTQUEUE_MCSP

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

#if defined(_MSC_VER)

#define FORCE_INLINE __forceinline
#define NEVER_INLINE __declspec(noinline)

#else

#define FORCE_INLINE inline __attribute__((always_inline))
#define NEVER_INLINE __attribute__((noinline))

#endif

template<typename T>
concept ObjectQueueFreeable = requires(T *obj) {
    /// used by the writer thread to know if the given memory is free and can be reused
    { OQ_IsObjectFree(obj) } -> std::same_as<bool>;

    /// used by the reader threads to 'mark' the memory as freed after the object in it is destroyed
    { OQ_FreeObject(obj) } -> std::same_as<void>;
};

template<typename ObjectType, bool isWriteProtected>
requires ObjectQueueFreeable<ObjectType>

class ObjectQueue_MCSP {
public:
    class Index {
    public:
        Index() noexcept = default;

        Index(Index const &) noexcept = default;

        friend bool operator==(Index const &l, Index const &r) noexcept {
            return l.value == r.value && l.use_count == r.use_count;
        }

        uint32_t getValue() const noexcept { return value; }

        uint32_t getCount() const noexcept { return use_count; }

        Index getIncrCount(uint32_t new_value) const noexcept { return {new_value, use_count + 1}; }

        Index getSameCount(uint32_t new_value) const noexcept { return {new_value, use_count}; }

    private:
        Index(uint32_t value, uint32_t index) noexcept : value{value}, use_count{index} {}

        alignas(std::atomic<uint64_t>) uint32_t value{};
        uint32_t use_count{};
    };

    class Ptr {
    public:
        Ptr(Ptr &&other) noexcept : object_ptr{std::exchange(other.object_ptr, nullptr)} {}

        Ptr(Ptr const &) = delete;

        Ptr &operator=(Ptr &&other) noexcept {
            this->~Ptr();
            object_ptr = std::exchange(other.object_ptr, nullptr);
            return *this;
        }

        operator bool() const noexcept { return object_ptr; }

        ObjectType &operator*() const noexcept { return *object_ptr; }

        ObjectType *get() const noexcept { return object_ptr; }

        ~Ptr() noexcept {
            if (object_ptr) {
                std::destroy_at(object_ptr);
                OQ_FreeObject(object_ptr);/// atomically reset object's memory to free status
            }
        }

    private:
        friend class ObjectQueue_MCSP;

        explicit Ptr(ObjectType *object) noexcept : object_ptr{object} {}

        ObjectType *object_ptr;
    };

public:
    ObjectQueue_MCSP(ObjectType *buffer, size_t count)
        : m_Array{buffer}, m_LastElementIndex{static_cast<uint32_t>(count) - 1} {
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
    }

    ~ObjectQueue_MCSP() noexcept { destroyAllObjects(); }

    uint32_t size() const noexcept {
        auto const output_index = m_OutputHeadIndex.load(std::memory_order::relaxed);
        auto const input_index = m_InputIndex.load(std::memory_order::acquire);
        if (input_index >= output_index) return input_index - output_index;
        else
            return m_LastElementIndex - output_index + 1 + input_index;
    }

    bool empty() const noexcept {
        return m_InputIndex.load(std::memory_order::acquire) == m_OutputHeadIndex.load(std::memory_order::acquire);
    }

    Ptr consume() const noexcept {
        auto const obj_index = get_index();
        return obj_index != INVALID_INDEX ? Ptr{m_Array + obj_index} : Ptr{nullptr};
    }

    template<typename Functor>
    bool consume(Functor &&functor) const noexcept {
        auto const obj_index = get_index();
        if (obj_index != INVALID_INDEX) {
            std::forward<Functor>(functor)(m_Array[obj_index]);
            destroy(obj_index);
            return true;
        } else
            return false;
    }

    template<typename Functor>
    uint32_t consume_all(Functor &&functor) const noexcept {
        uint32_t consumed{0};
        while (true) {
            auto const obj_index = get_index();
            if (obj_index != INVALID_INDEX) {
                std::forward<Functor>(functor)(m_Array[obj_index]);
                destroy(obj_index);
                ++consumed;
            } else
                return consumed;
        }
    }

    template<typename Functor>
    uint32_t consume_n(Functor &&functor) const noexcept {
        uint32_t consumed{0};
        while (true) {
            auto const obj_index = get_index();
            if (obj_index != INVALID_INDEX) {
                bool const consume_more = std::forward<Functor>(functor)(m_Array[obj_index]);
                destroy(obj_index);
                ++consumed;

                if (!consume_more) return consumed;
            } else
                return consumed;
        }
    }

    template<typename T>
    requires std::same_as<std::decay_t<T>, ObjectType>
    bool push_back(T &&obj) noexcept { return emplace_back(std::forward<T>(obj)); }

    template<typename... Args>
    bool emplace_back(Args &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const index_value = input_index.getValue();
        auto const output_index = cleanMemory();
        auto const next_input_index = index_value == m_LastElementIndex ? 0 : (index_value + 1);

        if (next_input_index == output_index) return false;

        std::construct_at(m_Array + index_value, std::forward<Args>(args)...);

        m_InputIndex.store(input_index.getIncrCount(next_input_index), std::memory_order::release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order::release); }

        return true;
    }

    template<typename Functor>
    uint32_t emplace_back_n(Functor &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        auto const input_index = m_InputIndex.load(std::memory_order::relaxed);
        auto const index_value = input_index.getValue();
        auto const output_index = cleanMemory();
        auto const count_avl = (index_value < output_index)
                                       ? (output_index - index_value - 1)
                                       : (m_LastElementIndex - index_value + static_cast<bool>(output_index));

        if (!count_avl) return 0;

        auto const count_emplaced = std::forward<Functor>(functor)(m_Array + index_value, count_avl);
        auto const input_end = index_value + count_emplaced;
        auto const next_input_index = (input_end == (m_LastElementIndex + 1)) ? 0 : input_end;

        m_InputIndex.store(input_index.getIncrCount(next_input_index), std::memory_order::release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order::release); }

        return count_emplaced;
    }

private:
    FORCE_INLINE uint32_t get_index() const noexcept {
        auto next_index = [&](uint32_t index) { return index == m_LastElementIndex ? 0 : index + 1; };

        auto output_index = m_OutputHeadIndex.load(std::memory_order::relaxed);
        Index nextOutputIndex;

        do {
            auto const input_index = m_InputIndex.load(std::memory_order::acquire);

            if (input_index.getCount() < output_index.getCount()) return INVALID_INDEX;
            if (input_index.getValue() == output_index.getValue()) return INVALID_INDEX;

            nextOutputIndex = input_index.getSameCount(next_index(output_index.getValue()));

        } while (!m_OutputHeadIndex.compare_exchange_weak(output_index, nextOutputIndex, std::memory_order::relaxed,
                                                          std::memory_order::relaxed));

        return output_index.getValue();
    }

    FORCE_INLINE uint32_t cleanMemory() noexcept {
        // this is the only function that modifies m_OutputTailIndex

        auto const output_tail = m_OutputTailIndex;
        auto const output_head = m_OutputHeadIndex.load(std::memory_order::acquire).getValue();

        auto checkAndForwardIndex = [this](uint32_t tail, uint32_t end) {
            for (; tail != end && OQ_IsObjectFree(m_Array + tail); ++tail)
                ;
            return tail;
        };

        if (output_tail == output_head) return output_tail;
        else if (output_tail < output_head) {
            auto const new_tail = checkAndForwardIndex(output_tail, output_head);
            if (new_tail != output_tail) m_OutputTailIndex = new_tail;

            return new_tail;
        } else {
            auto const array_end = m_LastElementIndex + 1;
            auto new_tail = checkAndForwardIndex(output_tail, array_end);
            if (new_tail == array_end) new_tail = checkAndForwardIndex(0, output_head);
            m_OutputTailIndex = new_tail;

            return new_tail;
        }
    }

    void destroyAllObjects() noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) {
            auto const input_index = m_InputIndex.load(std::memory_order::relaxed).getValue();
            auto const output_tail = m_OutputTailIndex;
            auto const output_head = m_OutputHeadIndex.load(std::memory_order::acquire).getValue();

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

    FORCE_INLINE void destroy(uint32_t index) const noexcept {
        if constexpr (!std::is_trivially_destructible_v<ObjectType>) { std::destroy_at(m_Array + index); }
        // atomically reset object's memory to free status
        OQ_FreeObject(m_Array + index);
    }

private:
    class Null {
    public:
        template<typename... T>
        explicit Null(T &&...) noexcept {}
    };

    static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();

    std::atomic<Index> m_InputIndex{};
    uint32_t m_OutputTailIndex{0};
    mutable std::atomic<Index> m_OutputHeadIndex{};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;

    uint32_t const m_LastElementIndex;
    ObjectType *const m_Array;
};

#endif
