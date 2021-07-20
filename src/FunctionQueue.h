#ifndef FUNCTIONQUEUE
#define FUNCTIONQUEUE

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

template<typename T, bool destroyNonInvoked = true>
class FunctionQueue {};

template<typename R, typename... Args, bool destroyNonInvoked>
class FunctionQueue<R(Args...), destroyNonInvoked> {
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
        using InvokeAndDestroy = R (*)(void *obj_ptr, Args...) noexcept;
        using Destroy = void (*)(void *obj_ptr) noexcept;

        auto getReadData() const noexcept {
            return std::tuple{std::bit_cast<InvokeAndDestroy>(getFp(fp_offset)),
                              std::bit_cast<std::byte *>(this) + obj_offset, getNextAddr()};
        }

        template<typename = void>
        requires(destroyNonInvoked) void destroyFO() const noexcept {
            std::bit_cast<Destroy>(getFp(destroyFp_offset))(std::bit_cast<std::byte *>(this) + obj_offset);
        }

        std::byte *getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + stride; }

    private:
        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept
            : fp_offset{getFpOffset(&invokeAndDestroy<Callable>)}, destroyFp_offset{getFpOffset(&destroy<Callable>)},
              obj_offset{obj_offset}, stride{stride} {}

        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Empty> destroyFp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return fp_ptr && obj_ptr; }

        template<typename Callable, typename... CArgs>
        std::byte *construct(CArgs &&...args) const noexcept {
            uint16_t const obj_offset = std::bit_cast<std::byte *>(obj_ptr) - std::bit_cast<std::byte *>(fp_ptr);
            new (fp_ptr)
                    FunctionContext{Type<Callable>{}, obj_offset, static_cast<uint16_t>(obj_offset + sizeof(Callable))};
            new (obj_ptr) Callable{std::forward<CArgs>(args)...};

            return std::bit_cast<std::byte *>(obj_ptr) + sizeof(Callable);
        }

        static Storage getAlignedStorage(void *buffer, size_t size, size_t obj_align, size_t obj_size) noexcept {
            auto const fp_storage = std::align(alignof(FunctionContext), sizeof(FunctionContext), buffer, size);
            buffer = std::bit_cast<std::byte *>(buffer) + sizeof(FunctionContext);
            size -= sizeof(FunctionContext);
            auto const callable_storage = std::align(obj_align, obj_size, buffer, size);
            return {fp_storage, callable_storage};
        }

    private:
        Storage(void *fp_ptr, void *obj_ptr) noexcept
            : fp_ptr{static_cast<FunctionContext *>(fp_ptr)}, obj_ptr{obj_ptr} {}

        FunctionContext *const fp_ptr{};
        void *const obj_ptr{};
    };

public:
    FunctionQueue(std::byte *memory, std::size_t size) noexcept
        : m_BufferSize{static_cast<uint32_t>(size)}, m_Buffer{memory} {}

    uint32_t buffer_size() const noexcept { return m_BufferSize; }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputOffset = 0;
        m_OutputOffset = 0;
        m_SentinelRead = NO_SENTINEL;
    }

    ~FunctionQueue() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();
    }

    bool empty() const noexcept { return m_InputOffset == m_OutputOffset; }

    FORCE_INLINE R call_and_pop(Args... args) const noexcept {
        auto const output_offset = m_OutputOffset;
        bool const found_sentinel = output_offset == m_SentinelRead;
        if (found_sentinel) { m_SentinelRead = NO_SENTINEL; }

        auto const functionCxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset));
        auto const [invokeAndDestroyFP, objPtr, nextAddr] = functionCxtPtr->getReadData();
        uint32_t const next_output_offset = nextAddr - m_Buffer;

        if constexpr (std::is_void_v<R>) {
            invokeAndDestroyFP(objPtr, args...);
            m_OutputOffset = next_output_offset;
        } else {
            auto &&result{invokeAndDestroyFP(objPtr, args...)};
            m_OutputOffset = next_output_offset;
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        using Callable = std::remove_cvref_t<T>;
        return emplace_back<Callable>(std::forward<T>(function));
    }

    template<typename Callable, typename... CArgs>
    bool emplace_back(CArgs &&...args) noexcept {
        auto const storage = getStorage(alignof(Callable), sizeof(Callable));
        if (!storage) return false;

        uint32_t const next_input_offset =
                storage.template construct<Callable>(std::forward<CArgs>(args)...) - m_Buffer;

        m_InputOffset = next_input_offset;

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
        auto const input_offset = m_InputOffset;
        auto output_offset = m_OutputOffset;

        auto destroyAndGetNext = [this](uint32_t offset) {
            auto const functionCxtPtr = align<FunctionContext>(m_Buffer + offset);
            functionCxtPtr->destroyFO();
            return functionCxtPtr->getNextAddr() - m_Buffer;
        };

        if (input_offset == output_offset) return;
        else if (output_offset > input_offset) {
            auto const sentinel = m_SentinelRead;
            while (output_offset != sentinel) { output_offset = destroyAndGetNext(output_offset); }
            output_offset = 0;
        }

        while (output_offset != input_offset) { output_offset = destroyAndGetNext(output_offset); }
    }

    FORCE_INLINE Storage getStorage(size_t obj_align, size_t const obj_size) noexcept {
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutputOffset;

        if (input_offset >= output_offset) {
            if (auto const storage = Storage::getAlignedStorage(m_Buffer + input_offset,
                                                                m_BufferSize - input_offset - 1, obj_align, obj_size)) {
                return storage;
            }

            if (output_offset) {
                if (auto const storage = Storage::getAlignedStorage(m_Buffer, output_offset - 1, obj_align, obj_size)) {
                    m_SentinelRead = input_offset;
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

    uint32_t m_InputOffset{0};
    mutable uint32_t m_OutputOffset{0};
    mutable uint32_t m_SentinelRead{NO_SENTINEL};

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;
};

#endif
