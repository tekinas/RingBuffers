#ifndef FUNCTIONQUEUE_SCSP
#define FUNCTIONQUEUE_SCSP

#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

template<typename T, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked = true>
class FunctionQueue_SCSP {};

template<typename R, typename... Args, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked>
class FunctionQueue_SCSP<R(Args...), isReadProtected, isWriteProtected, destroyNonInvoked> {
private:
    using InvokeAndDestroy = R (*)(void *obj_ptr, Args...) noexcept;
    using Destroy = void (*)(void *obj_ptr) noexcept;

    class Null {
    public:
        template<typename... T>
        explicit Null(T &&...) noexcept {}
    };

    template<typename>
    class Type {};

    struct Storage;

    struct FunctionContext {
    public:
        inline auto getReadData() noexcept {
            return std::tuple{std::bit_cast<InvokeAndDestroy>(fp_offset + fp_base),
                              std::bit_cast<std::byte *>(this) + obj_offset, getNextAddr()};
        }

        template<typename = void>
        requires(destroyNonInvoked) inline void destroyFO() noexcept {
            std::bit_cast<Destroy>(destroyFp_offset + fp_base)(std::bit_cast<std::byte *>(this) + obj_offset);
        }

        inline auto getNextAddr() noexcept { return std::bit_cast<std::byte *>(this) + stride; }

    private:
        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept
            : fp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(&invokeAndDestroy<Callable>) - fp_base)},
              destroyFp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(&destroy<Callable>) - fp_base)},
              obj_offset{obj_offset}, stride{stride} {}

        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Null> destroyFp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        Storage(void *fp_ptr, void *obj_ptr, uint32_t input_offset) noexcept
            : fp_ptr{static_cast<FunctionContext *>(fp_ptr)}, obj_ptr{obj_ptr}, next_input_offset{input_offset} {}

        inline explicit operator bool() const noexcept { return fp_ptr && obj_ptr; }

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
        uint32_t const next_input_offset{};
    };

public:
    FunctionQueue_SCSP(void *memory, std::size_t size) noexcept
        : m_Buffer{static_cast<std::byte *>(memory)}, m_BufferSize{static_cast<uint32_t>(size)} {
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
    }

    ~FunctionQueue_SCSP() noexcept {
        if constexpr (destroyNonInvoked) { destroyAllFO(); }
    }

    inline auto buffer_size() const noexcept { return m_BufferSize; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order_acquire) == m_OutputOffset.load(std::memory_order_acquire);
    }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputOffset.store(0, std::memory_order_relaxed);
        m_OutputOffset.store(0, std::memory_order_relaxed);
        m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
    }

    inline bool reserve() noexcept {
        if constexpr (isReadProtected) {
            if (m_ReadFlag.test_and_set(std::memory_order_relaxed)) return false;
            if (empty()) {
                m_ReadFlag.clear(std::memory_order_relaxed);
                return false;
            } else
                return true;
        } else
            return !empty();
    }

    inline R call_and_pop(Args... args) noexcept {
        auto const output_offset = m_OutputOffset.load(std::memory_order_relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order_relaxed);
        if (found_sentinel) { m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed); }

        auto const functionCxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset));

        auto const [invokeAndDestroyFP, objPtr, nextAddr] = functionCxtPtr->getReadData();

        auto decr_rem_incr_output_offset = [this, offset{nextAddr - m_Buffer}] {
            auto const next_output_offset = offset != m_BufferSize ? offset : 0;
            if constexpr (isReadProtected) {
                m_OutputOffset.store(next_output_offset, std::memory_order_release);
                m_ReadFlag.clear(std::memory_order_release);
            } else
                m_OutputOffset.store(next_output_offset, std::memory_order_release);
        };

        if constexpr (std::is_same_v<void, R>) {
            invokeAndDestroyFP(objPtr, args...);
            decr_rem_incr_output_offset();
        } else {
            auto &&result{invokeAndDestroyFP(objPtr, args...)};
            decr_rem_incr_output_offset();
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename T>
    inline bool push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;
        return emplace_back<Callable>(std::forward<T>(function));
    }

    template<typename Callable, typename... CArgs>
    inline bool emplace_back(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const storage = getStorage(alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }
            return false;
        }

        auto const next_input_offset = storage.template construct_callable<Callable>(std::forward<CArgs>(args)...);

        m_InputOffset.store(next_input_offset, std::memory_order_release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }

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
        if constexpr (std::is_same_v<R, void>) {
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

    template<typename = void>
    requires(destroyNonInvoked) void destroyAllFO() {
        auto const input_offset = m_InputOffset.load(std::memory_order_acquire);
        auto output_offset = m_OutputOffset.load(std::memory_order_acquire);

        auto destroyAndGetNext = [this](uint32_t offset) {
            auto const functionCxtPtr = align<FunctionContext>(m_Buffer + offset);
            functionCxtPtr->destroyFO();
            return functionCxtPtr->getNextAddr() - m_Buffer;
        };

        if (input_offset == output_offset) return;
        if (output_offset > input_offset) {
            auto sentinel = m_SentinelRead.load(std::memory_order_relaxed);
            if (sentinel == NO_SENTINEL) sentinel = m_BufferSize;
            while (output_offset != sentinel) { output_offset = destroyAndGetNext(output_offset); }
            output_offset = 0;
        }

        while (output_offset != input_offset) { output_offset = destroyAndGetNext(output_offset); }
    }

    inline Storage __attribute__((always_inline)) getStorage(size_t obj_align, size_t const obj_size) noexcept {
        auto get_next_input_offset = [&](void *callable_ptr) -> uint32_t {
            return std::bit_cast<std::byte *>(callable_ptr) - m_Buffer + obj_size;
        };

        auto get_aligned_ptrs = [obj_align, obj_size](void *buffer, size_t size) noexcept -> std::pair<void *, void *> {
            auto const fp_storage = std::align(alignof(FunctionContext), sizeof(FunctionContext), buffer, size);
            buffer = std::bit_cast<std::byte *>(buffer) + sizeof(FunctionContext);
            size -= sizeof(FunctionContext);
            auto const callable_storage = std::align(obj_align, obj_size, buffer, size);
            return {fp_storage, callable_storage};
        };

        auto const input_offset = m_InputOffset.load(std::memory_order_relaxed);
        auto const output_offset = m_OutputOffset.load(std::memory_order_acquire);

        if (input_offset >= output_offset) {
            if (auto const ptr_pair = get_aligned_ptrs(m_Buffer + input_offset, m_BufferSize - input_offset);
                ptr_pair.first && ptr_pair.second) {
                auto const next_input_offset = get_next_input_offset(ptr_pair.second);
                if (next_input_offset < m_BufferSize) return {ptr_pair.first, ptr_pair.second, next_input_offset};
                else if (next_input_offset == m_BufferSize && output_offset) {
                    return {ptr_pair.first, ptr_pair.second, 0};
                }
            }

            if (output_offset)
                if (auto const ptr_pair = get_aligned_ptrs(m_Buffer, output_offset - 1);
                    ptr_pair.first && ptr_pair.second) {
                    m_SentinelRead.store(input_offset, std::memory_order_relaxed);
                    return {ptr_pair.first, ptr_pair.second, get_next_input_offset(ptr_pair.second)};
                }

        } else {
            auto const ptr_pair = get_aligned_ptrs(m_Buffer + input_offset, output_offset - input_offset - 1);
            return {ptr_pair.first, ptr_pair.second, get_next_input_offset(ptr_pair.second)};
        }

        return {};
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    std::atomic<uint32_t> m_InputOffset{0};
    std::atomic<uint32_t> m_OutputOffset{0};
    std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;
    [[no_unique_address]] std::conditional_t<isReadProtected, std::atomic_flag, Null> m_ReadFlag;

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;

    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_same_v<R, void>) { return R{}; }
    }

    static inline const uintptr_t fp_base = std::bit_cast<uintptr_t>(&invokeAndDestroy<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);
};

#endif// FUNCTIONQUEUE_SCSP
