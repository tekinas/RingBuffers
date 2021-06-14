#ifndef FUNCTIONQUEUE_SCSP
#define FUNCTIONQUEUE_SCSP

#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>

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
    private:
        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Null> destroyFp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        friend class Storage;

        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept
            : fp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(&invokeAndDestroy<Callable>) - fp_base)},
              destroyFp_offset{static_cast<uint32_t>(std::bit_cast<uintptr_t>(&destroy<Callable>) - fp_base)},
              obj_offset{obj_offset}, stride{stride} {}

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
    };

    struct Storage {
        FunctionContext *const fp_ptr{};
        void *const obj_ptr{};

        Storage() noexcept = default;

        Storage(void *fp_ptr, void *obj_ptr) noexcept
            : fp_ptr{static_cast<FunctionContext *>(fp_ptr)}, obj_ptr{obj_ptr} {}

        inline explicit operator bool() const noexcept { return fp_ptr && obj_ptr; }

        template<typename Callable, typename... CArgs>
        inline void construct_callable(CArgs &&...args) const noexcept {
            new (fp_ptr) FunctionContext{Type<Callable>{}, getObjOffset(),
                                         static_cast<uint16_t>(getObjOffset() + sizeof(Callable))};
            new (obj_ptr) Callable{std::forward<CArgs>(args)...};
        }

    private:
        [[nodiscard]] inline uint16_t getObjOffset() const noexcept {
            return std::bit_cast<std::byte *>(obj_ptr) - std::bit_cast<std::byte *>(fp_ptr);
        }
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

    inline auto size() const noexcept { return m_Remaining.load(std::memory_order_relaxed); }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputOffset = 0;
        m_Remaining.store(0, std::memory_order_relaxed);
        m_OutPutOffset.store(0, std::memory_order_relaxed);
        m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
    }

    inline bool reserve() noexcept {
        if constexpr (isReadProtected) {
            if (m_ReadFlag.test_and_set(std::memory_order_relaxed)) return false;
            if (!m_Remaining.load(std::memory_order_acquire)) {
                m_ReadFlag.clear(std::memory_order_relaxed);
                return false;
            } else
                return true;
        } else
            return m_Remaining.load(std::memory_order_acquire);
    }

    inline R call_and_pop(Args... args) noexcept {
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order_relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);

        auto functionCxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset));

        auto const [invokeAndDestroyFP, objPtr, nextAddr] = functionCxtPtr->getReadData();

        auto decr_rem_incr_output_offset = [&, nextOffset{nextAddr - m_Buffer}] {
            m_Remaining.fetch_sub(1, std::memory_order_relaxed);
            if constexpr (isReadProtected) {
                m_OutPutOffset.store(nextOffset, std::memory_order_relaxed);
                m_ReadFlag.clear(std::memory_order_release);
            } else
                m_OutPutOffset.store(nextOffset, std::memory_order_release);
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
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        using Callable = std::decay_t<T>;

        auto const storage = getMemory(alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }
            return false;
        }

        storage.template construct_callable<Callable>(std::forward<T>(function));

        m_Remaining.fetch_add(1, std::memory_order_release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }

        return true;
    }

    template<typename Callable, typename... CArgs>
    inline bool emplace_back(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const storage = getMemory(alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order_release); }
            return false;
        }

        storage.template construct_callable<Callable>(std::forward<CArgs>(args)...);

        m_Remaining.fetch_add(1, std::memory_order_release);

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
        auto remaining = m_Remaining.exchange(0, std::memory_order_acquire);

        if (!remaining) return;

        auto output_pos = m_OutPutOffset.load(std::memory_order_relaxed);

        auto destroyAndForward = [&](auto functionCxt) noexcept {
            output_pos = functionCxt->getNextAddr() - m_Buffer;
            functionCxt->destroyFO();
        };

        if (auto const sentinel = m_SentinelRead.load(std::memory_order_relaxed); sentinel != NO_SENTINEL) {
            while (output_pos != sentinel) {
                destroyAndForward(align<FunctionContext>(m_Buffer + output_pos));
                --remaining;
            }
            output_pos = 0;
        }

        while (remaining--) { destroyAndForward(align<FunctionContext>(m_Buffer + output_pos)); }
    }

    inline Storage getMemory(size_t obj_align, size_t obj_size) noexcept {
        auto getAlignedStorage = [obj_align, obj_size](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = std::align(alignof(FunctionContext), sizeof(FunctionContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(FunctionContext);
            size -= sizeof(FunctionContext);
            auto const callable_storage = std::align(obj_align, obj_size, buffer, size);
            return {fp_storage, callable_storage};
        };

        auto incr_input_offset = [&](auto &storage) noexcept {
            m_InputOffset = std::bit_cast<std::byte *>(storage.obj_ptr) - m_Buffer + obj_size;
        };

        auto const remaining = m_Remaining.load(std::memory_order_acquire);
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);

        auto const search_ahead = (input_offset > output_offset) || (input_offset == output_offset && !remaining);

        if (size_t const buffer_size = m_BufferSize - input_offset; search_ahead && buffer_size) {
            if (auto storage = getAlignedStorage(m_Buffer + input_offset, buffer_size)) {
                incr_input_offset(storage);
                return storage;
            }
        }

        {
            auto const mem = search_ahead ? 0 : input_offset;
            if (size_t const buffer_size = output_offset - mem) {
                if (auto storage = getAlignedStorage(m_Buffer + mem, buffer_size)) {
                    if (search_ahead) { m_SentinelRead.store(input_offset, std::memory_order_relaxed); }
                    incr_input_offset(storage);
                    return storage;
                }
            }
        }

        return {};
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    uint32_t m_InputOffset{0};
    std::atomic<uint32_t> m_Remaining{0};
    std::atomic<uint32_t> m_OutPutOffset{0};
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
