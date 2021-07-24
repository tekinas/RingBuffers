#ifndef FUNCTIONQUEUE_SCSP
#define FUNCTIONQUEUE_SCSP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

template<typename T, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked = true>
class FunctionQueue_SCSP {};

template<typename R, typename... Args, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked>
class FunctionQueue_SCSP<R(Args...), isReadProtected, isWriteProtected, destroyNonInvoked> {
private:
    class Empty {
    public:
        template<typename... T>
        explicit Empty(T &&...) noexcept {}
    };

    template<typename>
    class Type {};

    class Storage;

    class FunctionContext {
    public:
        using InvokeAndDestroy = R (*)(void *callable_ptr, Args...) noexcept;
        using Destroy = void (*)(void *callable_ptr) noexcept;

        auto getReadData() const noexcept {
            return std::tuple{std::bit_cast<InvokeAndDestroy>(getFp(fp_offset)),
                              std::bit_cast<std::byte *>(this) + callable_offset, getNextAddr()};
        }

        template<typename = void>
        requires(destroyNonInvoked) void destroyFO() const noexcept {
            std::bit_cast<Destroy>(getFp(destroyFp_offset))(std::bit_cast<std::byte *>(this) + callable_offset);
        }

        std::byte *getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + stride; }

    private:
        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t callable_offset, uint16_t stride) noexcept
            : fp_offset{getFpOffset(&invokeAndDestroy<Callable>)}, destroyFp_offset{getFpOffset(&destroy<Callable>)},
              callable_offset{callable_offset}, stride{stride} {}

        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Empty> destroyFp_offset;
        uint16_t const callable_offset;
        uint16_t const stride;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return fc_ptr && callable_ptr; }

        template<typename Callable, typename... CArgs>
        std::byte *construct(CArgs &&...args) const noexcept {
            constexpr size_t callable_size = std::is_empty_v<Callable> ? 0 : sizeof(Callable);
            uint16_t const callable_offset =
                    std::bit_cast<std::byte *>(callable_ptr) - std::bit_cast<std::byte *>(fc_ptr);
            uint16_t const stride =
                    std::is_empty_v<Callable> ? sizeof(FunctionContext) : (callable_offset + callable_size);

            new (fc_ptr) FunctionContext{Type<Callable>{}, callable_offset, stride};
            new (callable_ptr) Callable{std::forward<CArgs>(args)...};

            return std::bit_cast<std::byte *>(fc_ptr) + stride;
        }

        static Storage getAlignedStorage(void *buffer, size_t buffer_size, size_t obj_align, size_t obj_size) noexcept {
            auto const fc_ptr = std::align(alignof(FunctionContext), sizeof(FunctionContext), buffer, buffer_size);
            buffer = std::bit_cast<std::byte *>(buffer) + sizeof(FunctionContext);
            buffer_size -= sizeof(FunctionContext);
            auto const callable_ptr = std::align(obj_align, obj_size, buffer, buffer_size);
            return {fc_ptr, callable_ptr};
        }

    private:
        Storage(void *fc_ptr, void *callable_ptr) noexcept
            : fc_ptr{static_cast<FunctionContext *>(fc_ptr)}, callable_ptr{callable_ptr} {}

        FunctionContext *const fc_ptr{};
        void *const callable_ptr{};
    };

public:
    FunctionQueue_SCSP(std::byte *memory, std::size_t size) noexcept
        : m_BufferSize{static_cast<uint32_t>(size)}, m_Buffer{memory} {}

    ~FunctionQueue_SCSP() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();
    }

    uint32_t buffer_size() const noexcept { return m_BufferSize; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire) == m_OutputOffset.load(std::memory_order::acquire);
    }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputOffset.store(0, std::memory_order::relaxed);
        m_OutputOffset.store(0, std::memory_order::relaxed);
        m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::relaxed);
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

    R call_and_pop(Args... args) const noexcept {
        auto const output_offset = m_OutputOffset.load(std::memory_order::relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order::relaxed);
        if (found_sentinel) { m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed); }

        auto const functionCxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset));
        auto const [invokeAndDestroyFP, objPtr, nextAddr] = functionCxtPtr->getReadData();

        auto incr_output_offset = [this, next_output_offset{nextAddr - m_Buffer}] {
            m_OutputOffset.store(next_output_offset, std::memory_order::release);
            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);
        };

        if constexpr (std::is_void_v<R>) {
            invokeAndDestroyFP(objPtr, args...);
            incr_output_offset();
        } else {
            auto &&result{invokeAndDestroyFP(objPtr, args...)};
            incr_output_offset();
            return std::forward<decltype(result)>(result);
        }
    }

    template<R (*function)(Args...)>
    bool push_back() noexcept {
        return push_back([](Args... args) { return function(std::forward<Args>(args)...); });
    }

    template<typename T>
    bool push_back(T &&callable) noexcept {
        using NoCVRef_t = std::remove_cvref_t<T>;
        using Callable = std::conditional_t<std::is_function_v<NoCVRef_t>, std::add_pointer_t<NoCVRef_t>, NoCVRef_t>;
        return emplace_back<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    bool emplace_back(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        auto const storage = getStorage(alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
            return false;
        }

        uint32_t const next_input_offset =
                storage.template construct<Callable>(std::forward<CArgs>(args)...) - m_Buffer;

        m_InputOffset.store(next_input_offset, std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);

        return true;
    }

private:
    template<typename T>
    static constexpr T *align(void const *ptr) noexcept {
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

    template<typename = void>
    requires(destroyNonInvoked) void destroyAllFO() {
        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);
        auto output_offset = m_OutputOffset.load(std::memory_order::acquire);

        auto destroyAndGetNext = [this](uint32_t offset) {
            auto const functionCxtPtr = align<FunctionContext>(m_Buffer + offset);
            functionCxtPtr->destroyFO();
            return functionCxtPtr->getNextAddr() - m_Buffer;
        };

        if (input_offset == output_offset) return;
        else if (output_offset > input_offset) {
            auto const sentinel = m_SentinelRead.load(std::memory_order::relaxed);
            while (output_offset != sentinel) { output_offset = destroyAndGetNext(output_offset); }
            output_offset = 0;
        }

        while (output_offset != input_offset) { output_offset = destroyAndGetNext(output_offset); }
    }

    FORCE_INLINE Storage getStorage(size_t obj_align, size_t const obj_size) noexcept {
        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const output_offset = m_OutputOffset.load(std::memory_order::acquire);

        if (input_offset >= output_offset) {
            if (auto const storage = Storage::getAlignedStorage(m_Buffer + input_offset,
                                                                m_BufferSize - input_offset - 1, obj_align, obj_size)) {
                return storage;
            }

            if (output_offset) {
                if (auto const storage = Storage::getAlignedStorage(m_Buffer, output_offset - 1, obj_align, obj_size)) {
                    m_SentinelRead.store(input_offset, std::memory_order::relaxed);
                    return storage;
                }
            }
            return {};
        } else
            return Storage::getAlignedStorage(m_Buffer + input_offset, output_offset - input_offset - 1, obj_align,
                                              obj_size);
    }

    static void fp_base_func() noexcept {}

    static void *getFp(uint32_t fp_offset) noexcept {
        const uintptr_t fp_base =
                std::bit_cast<uintptr_t>(&fp_base_func) & (static_cast<uintptr_t>(0XFFFFFFFF00000000lu));
        return std::bit_cast<void *>(fp_base + fp_offset);
    }

    template<typename FR, typename... FArgs>
    static uint32_t getFpOffset(FR (*fp)(FArgs...)) noexcept {
        return static_cast<uint32_t>(std::bit_cast<uintptr_t>(fp));
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    std::atomic<uint32_t> m_InputOffset{0};
    mutable std::atomic<uint32_t> m_OutputOffset{0};
    mutable std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Empty> m_WriteFlag{};
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, Empty> m_ReadFlag{};

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;
};

#endif
