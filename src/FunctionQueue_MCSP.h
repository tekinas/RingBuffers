#ifndef FUNCTIONQUEUE_FUNCTIONQUEUE_MCSP_H
#define FUNCTIONQUEUE_FUNCTIONQUEUE_MCSP_H

#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

template<typename T, bool isWriteProtected, bool destroyNonInvoked = true>
class FunctionQueue_MCSP {};

template<typename R, typename... Args, bool isWriteProtected, bool destroyNonInvoked>
class FunctionQueue_MCSP<R(Args...), isWriteProtected, destroyNonInvoked> {
private:
    using InvokeAndDestroy = R (*)(void *obj_ptr, Args...) noexcept;
    using Destroy = void (*)(void *obj_ptr) noexcept;

    class Null {
    public:
        template<typename... T>
        explicit Null(T &&...) noexcept {}
    };

    class Offset {
    public:
        Offset() noexcept = default;

        Offset(Offset const &) noexcept = default;

        friend bool operator==(Offset const &l, Offset const &r) noexcept {
            return l.value == r.value && l.index == r.index;
        }

        uint32_t getValue() const noexcept { return value; }

        uint32_t getIndex() const noexcept { return index; }

        Offset getIncrIndexed(uint32_t new_value) const noexcept { return {new_value, index + 1}; }

        Offset getSameIndexed(uint32_t new_value) const noexcept { return {new_value, index}; }

    private:
        Offset(uint32_t value, uint32_t index) noexcept : value{value}, index{index} {}

        alignas(uint64_t) uint32_t value{};
        uint32_t index{};
    };

    template<typename>
    class Type {};

    struct Storage;

    class FunctionContext {
    public:
        inline auto getReadData() const noexcept {
            return std::pair{std::bit_cast<InvokeAndDestroy>(fp_offset.load(std::memory_order::relaxed) + fp_base),
                             std::bit_cast<std::byte *>(this) + obj_offset};
        }

        template<typename = void>
        requires(destroyNonInvoked) inline void destroyFO() const noexcept {
            if (fp_offset.load(std::memory_order::acquire)) {
                std::bit_cast<Destroy>(destroyFp_offset + fp_base)(std::bit_cast<std::byte *>(this) + obj_offset);
            }
        }

        inline auto getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + stride; }

        inline void free() noexcept { fp_offset.store(0, std::memory_order::release); }

        [[nodiscard]] inline bool isFree() const noexcept { return fp_offset.load(std::memory_order::acquire) == 0; }

    private:
        template<typename Callable>
        inline FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept
            : fp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(&invokeAndDestroy<Callable>) - fp_base)},
              destroyFp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(&destroy<Callable>) - fp_base)},
              obj_offset{obj_offset}, stride{stride} {}

        std::atomic<uint32_t> fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Null> destroyFp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        Storage(void *fp_ptr, void *obj_ptr) noexcept
            : fp_ptr{std::bit_cast<FunctionContext *>(fp_ptr)}, obj_ptr{obj_ptr} {}

        inline explicit operator bool() const noexcept { return fp_ptr && obj_ptr; }

        void set_input_offset(uint32_t input_offset) noexcept { next_input_offset = input_offset; }

        std::byte *getNextAddr(size_t obj_size) const noexcept {
            return std::bit_cast<std::byte *>(obj_ptr) + obj_size;
        }

        template<typename Callable, typename... CArgs>
        inline uint32_t construct_callable(CArgs &&...args) const noexcept {
            uint16_t const obj_offset = std::bit_cast<std::byte *>(obj_ptr) - std::bit_cast<std::byte *>(fp_ptr);
            new (fp_ptr)
                    FunctionContext{Type<Callable>{}, obj_offset, static_cast<uint16_t>(obj_offset + sizeof(Callable))};
            new (obj_ptr) Callable{std::forward<CArgs>(args)...};
            return next_input_offset;
        }

    private:
        FunctionContext *const fp_ptr{};
        void *const obj_ptr{};
        uint32_t next_input_offset{};
    };


public:
    class FunctionHandle {
    public:
        FunctionHandle() noexcept = default;

        FunctionHandle(FunctionHandle &&other) noexcept
            : functionCxtPtr{std::exchange(other.functionCxtPtr, nullptr)} {}

        FunctionHandle(FunctionHandle const &) = delete;

        operator bool() const noexcept { return functionCxtPtr; }

        R call_and_pop(Args... args) noexcept {
            auto const [invokeAndDestroyFP, objPtr] = functionCxtPtr->getReadData();
            auto clean_up = [&] {
                functionCxtPtr->free();
                functionCxtPtr = nullptr;
            };

            if constexpr (std::is_void_v<R>) {
                invokeAndDestroyFP(objPtr, std::forward<Args>(args)...);
                clean_up();
            } else {
                auto &&result{invokeAndDestroyFP(objPtr, std::forward<Args>(args)...)};
                clean_up();
                return std::forward<decltype(result)>(result);
            }
        }

        FunctionHandle &operator=(FunctionHandle &&other) noexcept {
            this->~FunctionHandle();
            functionCxtPtr = std::exchange(other.functionCxtPtr, nullptr);
            return *this;
        }

        ~FunctionHandle() noexcept {
            if (functionCxtPtr) {
                if constexpr (destroyNonInvoked) functionCxtPtr->destroyFO();
                functionCxtPtr->free();
            }
        }

    private:
        friend class FunctionQueue_MCSP;

        explicit FunctionHandle(FunctionContext *fcp) noexcept : functionCxtPtr{fcp} {}

        FunctionContext *functionCxtPtr{nullptr};
    };

    FunctionQueue_MCSP(std::byte *memory, std::size_t size) noexcept
        : m_Buffer{memory}, m_BufferSize{static_cast<uint32_t>(size)} {
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
    }

    ~FunctionQueue_MCSP() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();
    }

    inline auto buffer_size() const noexcept { return m_BufferSize; }

    inline bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire) == m_OutputHeadOffset.load(std::memory_order::relaxed);
    }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputOffset.store(0, std::memory_order_relaxed);
        m_OutputTailOffset = 0;
        m_SentinelFollow = NO_SENTINEL;

        m_OutputHeadOffset.store(0, std::memory_order::relaxed);
        m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
    }

    FunctionHandle __attribute__((always_inline)) get_function_handle() noexcept {
        bool found_sentinel;
        FunctionContext *fcxtPtr;
        Offset nextOutputOffset;

        auto output_offset = m_OutputHeadOffset.load(std::memory_order::relaxed);

        do {
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

            if (input_offset.getIndex() < output_offset.getIndex()) return FunctionHandle{nullptr};
            if (input_offset.getValue() == output_offset.getValue()) return FunctionHandle{nullptr};

            found_sentinel = output_offset.getValue() == m_SentinelRead.load(std::memory_order::relaxed);
            fcxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset.getValue()));

            uint32_t const offset = fcxtPtr->getNextAddr() - m_Buffer;
            uint32_t const nextOffset = offset != m_BufferSize ? offset : 0;
            nextOutputOffset = input_offset.getSameIndexed(nextOffset);

        } while (!m_OutputHeadOffset.compare_exchange_weak(output_offset, nextOutputOffset, std::memory_order::relaxed,
                                                           std::memory_order::relaxed));

        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
        return FunctionHandle{fcxtPtr};
    }

    /*FunctionHandle get_function_handle1() noexcept {
        auto output_offset = m_OutputHeadOffset.load(std::memory_order::relaxed);
        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

        if (input_offset.getIndex() < output_offset.getIndex()) return FunctionHandle{nullptr};
        if (input_offset.getValue() == output_offset.getValue()) return FunctionHandle{nullptr};

        bool const found_sentinel = output_offset.getValue() == m_SentinelRead.load(std::memory_order::relaxed);
        auto const fcxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset.getValue()));

        uint32_t const offset = fcxtPtr->getNextAddr() - m_Buffer;
        uint32_t const nextOffset = offset != m_BufferSize ? offset : 0;
        auto const nextOutputOffset = input_offset.getSameIndexed(nextOffset);

        if (m_OutputHeadOffset.compare_exchange_strong(output_offset, nextOutputOffset, std::memory_order::relaxed,
                                                       std::memory_order::relaxed)) {
            if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
            return FunctionHandle{fcxtPtr};
        } else
            return FunctionHandle{nullptr};
    }*/

    template<typename T>
    inline bool push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;
        return emplace_back<Callable>(std::forward<T>(function));
    }

    template<typename Callable, typename... CArgs>
    inline bool emplace_back(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected)
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const storage = getStorage(input_offset.getValue(), alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
            return false;
        }

        auto const next_input_offset = storage.template construct_callable<Callable>(std::forward<CArgs>(args)...);

        m_InputOffset.store(input_offset.getIncrIndexed(next_input_offset), std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);

        return true;
    }

private:
    template<typename T>
    static constexpr inline T *align(void *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + alignof(T)) & -alignof(T));
    }

    template<typename Callable>
    static R invokeAndDestroy(void *data, Args... args) noexcept {
        auto const functor_ptr = static_cast<Callable *>(data);
        if constexpr (std::is_void_v<R>) {
            (*functor_ptr)(std::forward<Args>(args)...);
            std::destroy_at(functor_ptr);
        } else {
            auto &&result{(*functor_ptr)(std::forward<Args>(args)...)};
            std::destroy_at(functor_ptr);
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
    }

    inline __attribute__((always_inline)) uint32_t cleanMemory() noexcept {
        auto const output_head = m_OutputHeadOffset.load(std::memory_order_relaxed).getValue();
        auto output_tail = m_OutputTailOffset;

        if (output_tail != output_head) {
            while (output_tail != output_head) {
                if (output_tail == m_SentinelFollow) {
                    if (m_SentinelRead.load(std::memory_order::relaxed) == NO_SENTINEL) {
                        output_tail = 0;
                        m_SentinelFollow = NO_SENTINEL;
                    } else
                        break;
                } else if (auto const functionCxt = align<FunctionContext>(m_Buffer + output_tail);
                           functionCxt->isFree()) {
                    output_tail = functionCxt->getNextAddr() - m_Buffer;
                    if (output_tail == m_BufferSize) output_tail = 0;
                } else {
                    break;
                }
            }
            m_OutputTailOffset = output_tail;
        }

        return m_OutputTailOffset;
    }

    template<typename = void>
    requires(destroyNonInvoked) void destroyAllFO() {
        auto const input_offset = m_InputOffset.load(std::memory_order_acquire).getValue();
        auto output_offset = cleanMemory();

        if (output_offset == input_offset) return;

        auto destroyAndGetNext = [&](uint32_t output_offset) noexcept -> uint32_t {
            auto const functionCxtPtr = align<FunctionContext>(m_Buffer + output_offset);
            auto const offset = functionCxtPtr->getNextAddr() - m_Buffer;
            functionCxtPtr->destroyFO();
            return offset;
        };

        if (output_offset > input_offset) {
            auto const sentinel = m_SentinelFollow != NO_SENTINEL ? m_SentinelFollow : m_BufferSize;
            while (output_offset != sentinel) { output_offset = destroyAndGetNext(output_offset); }
            output_offset = 0;
        }

        while (output_offset != input_offset) { output_offset = destroyAndGetNext(output_offset); }
    }

    inline Storage __attribute__((always_inline))
    getStorage(uint32_t input_offset, size_t obj_align, size_t const obj_size) noexcept {
        auto getNextInputOffset = [&](Storage &storage) -> uint32_t {
            return storage.getNextAddr(obj_size) - m_Buffer;
        };

        auto getAlignedStorage = [obj_align, obj_size](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = std::align(alignof(FunctionContext), sizeof(FunctionContext), buffer, size);
            buffer = std::bit_cast<std::byte *>(buffer) + sizeof(FunctionContext);
            size -= sizeof(FunctionContext);
            auto const callable_storage = std::align(obj_align, obj_size, buffer, size);
            return {fp_storage, callable_storage};
        };

        auto const output_offset = cleanMemory();

        if (input_offset >= output_offset) {
            if (auto storage = getAlignedStorage(m_Buffer + input_offset, m_BufferSize - input_offset)) {
                auto const next_input_offset = getNextInputOffset(storage);
                if (next_input_offset < m_BufferSize) {
                    storage.set_input_offset(next_input_offset);
                } else if (next_input_offset == m_BufferSize && output_offset) {
                    storage.set_input_offset(0);
                } else
                    return {};
                return storage;
            }

            if (output_offset)
                if (auto storage = getAlignedStorage(m_Buffer, output_offset - 1)) {
                    m_SentinelFollow = input_offset;
                    m_SentinelRead.store(input_offset, std::memory_order::relaxed);
                    storage.set_input_offset(getNextInputOffset(storage));
                    return storage;
                }

        } else {
            auto storage = getAlignedStorage(m_Buffer + input_offset, output_offset - input_offset - 1);
            storage.set_input_offset(getNextInputOffset(storage));
            return storage;
        }

        return {};
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    std::atomic<Offset> m_InputOffset{};
    uint32_t m_OutputTailOffset{0};
    uint32_t m_SentinelFollow{NO_SENTINEL};

    std::atomic<Offset> m_OutputHeadOffset{};
    std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;

    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_void_v<R>) return std::declval<R>();
    }

    static inline const uintptr_t fp_base = std::bit_cast<uintptr_t>(&invokeAndDestroy<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);
};

#endif
