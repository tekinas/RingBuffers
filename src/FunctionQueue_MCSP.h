#ifndef FUNCTIONQUEUE_MCSP
#define FUNCTIONQUEUE_MCSP

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

template<typename T, bool isWriteProtected, bool destroyNonInvoked = true>
class FunctionQueue_MCSP {};

template<typename R, typename... Args, bool isWriteProtected, bool destroyNonInvoked>
class FunctionQueue_MCSP<R(Args...), isWriteProtected, destroyNonInvoked> {
private:
    class TaggedUint32 {
    public:
        TaggedUint32() noexcept = default;

        friend bool operator==(TaggedUint32 const &l, TaggedUint32 const &r) noexcept {
            return l.value == r.value && l.tag == r.tag;
        }

        uint32_t getValue() const noexcept { return value; }

        uint32_t getTag() const noexcept { return tag; }

        TaggedUint32 getIncrTagged(uint32_t new_value) const noexcept { return {new_value, tag + 1}; }

        TaggedUint32 getSameTagged(uint32_t new_value) const noexcept { return {new_value, tag}; }

    private:
        TaggedUint32(uint32_t value, uint32_t index) noexcept : value{value}, tag{index} {}

        alignas(std::atomic<uint64_t>) uint32_t value{};
        uint32_t tag{};
    };

    template<typename>
    class Type {};

    class Empty {
    public:
        template<typename... T>
        explicit Empty(T &&...) noexcept {}
    };

    class Storage;

    class FunctionContext {
    public:
        using InvokeAndDestroy = R (*)(void *obj_ptr, Args...) noexcept;
        using Destroy = void (*)(void *obj_ptr) noexcept;

        auto getReadData() const noexcept {
            return std::pair{std::bit_cast<InvokeAndDestroy>(getFp(fp_offset.load(std::memory_order::relaxed))),
                             std::bit_cast<std::byte *>(this) + obj_offset};
        }

        template<typename = void>
        requires(destroyNonInvoked) void destroyFO() const noexcept {
            if (fp_offset.load(std::memory_order::acquire)) {
                std::bit_cast<Destroy>(getFp(destroyFp_offset))(std::bit_cast<std::byte *>(this) + obj_offset);
            }
        }

        std::byte *getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + stride; }

        void free() noexcept { fp_offset.store(0, std::memory_order::release); }

        [[nodiscard]] bool isFree() const noexcept { return fp_offset.load(std::memory_order::acquire) == 0; }

    private:
        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept
            : fp_offset{getFpOffset(&invokeAndDestroy<Callable>)}, destroyFp_offset{getFpOffset(&destroy<Callable>)},
              obj_offset{obj_offset}, stride{stride} {}

        std::atomic<uint32_t> fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Empty> destroyFp_offset;
        uint16_t const obj_offset;
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
            std::destroy_at(this);
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

        FunctionContext *functionCxtPtr{};
    };

    FunctionQueue_MCSP(std::byte *memory, size_t size) noexcept
        : m_BufferSize{static_cast<uint32_t>(size)}, m_Buffer{memory} {}

    ~FunctionQueue_MCSP() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();
    }

    uint32_t buffer_size() const noexcept { return m_BufferSize; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire) == m_OutputHeadOffset.load(std::memory_order::relaxed);
    }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputOffset.store({}, std::memory_order::relaxed);
        m_OutputTailOffset = 0;
        m_SentinelFollow = NO_SENTINEL;

        m_OutputHeadOffset.store({}, std::memory_order::relaxed);
        m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
    }

    FORCE_INLINE FunctionHandle get_function_handle() noexcept {
        bool found_sentinel;
        FunctionContext *fcxtPtr;
        TaggedUint32 nextOutputOffset;

        auto output_offset = m_OutputHeadOffset.load(std::memory_order::relaxed);

        do {
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

            if (input_offset.getTag() < output_offset.getTag()) return FunctionHandle{nullptr};
            if (input_offset.getValue() == output_offset.getValue()) return FunctionHandle{nullptr};

            found_sentinel = output_offset.getValue() == m_SentinelRead.load(std::memory_order::relaxed);
            fcxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset.getValue()));

            uint32_t const nextOffset = fcxtPtr->getNextAddr() - m_Buffer;
            nextOutputOffset = input_offset.getSameTagged(nextOffset);

        } while (!m_OutputHeadOffset.compare_exchange_weak(output_offset, nextOutputOffset, std::memory_order::relaxed,
                                                           std::memory_order::relaxed));

        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
        return FunctionHandle{fcxtPtr};
    }

    /*FunctionHandle get_function_handle1() noexcept {
        auto output_offset = m_OutputHeadOffset.load(std::memory_order::relaxed);
        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

        if (input_offset.getTag() < output_offset.getTag()) return FunctionHandle{nullptr};
        if (input_offset.getValue() == output_offset.getValue()) return FunctionHandle{nullptr};

        bool const found_sentinel = output_offset.getValue() == m_SentinelRead.load(std::memory_order::relaxed);
        auto const fcxtPtr = align<FunctionContext>(m_Buffer + (found_sentinel ? 0 : output_offset.getValue()));

        uint32_t const nextOffset = fcxtPtr->getNextAddr() - m_Buffer;
        auto const nextOutputOffset = input_offset.getSameTagged(nextOffset);

        if (m_OutputHeadOffset.compare_exchange_strong(output_offset, nextOutputOffset, std::memory_order::relaxed,
                                                       std::memory_order::relaxed)) {
            if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
            return FunctionHandle{fcxtPtr};
        } else
            return FunctionHandle{nullptr};
    }*/

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
        if constexpr (isWriteProtected)
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const storage = getStorage(input_offset.getValue(), alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
            return false;
        }

        auto const nextInputOffsetValue = storage.template construct<Callable>(std::forward<CArgs>(args)...) - m_Buffer;
        auto const nextInputOffset = input_offset.getIncrTagged(nextInputOffsetValue);

        m_InputOffset.store(nextInputOffset, std::memory_order::release);

        if (nextInputOffset.getTag() == 0) {
            auto output_offset = nextInputOffset;
            while (!m_OutputHeadOffset.compare_exchange_weak(output_offset,
                                                             nextInputOffset.getSameTagged(output_offset.getValue()),
                                                             std::memory_order::relaxed, std::memory_order::relaxed))
                ;
        }

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

    FORCE_INLINE uint32_t cleanMemory() noexcept {
        auto const output_head = m_OutputHeadOffset.load(std::memory_order::acquire).getValue();
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
                } else {
                    break;
                }
            }

            m_OutputTailOffset = output_tail;
        }

        return output_tail;
    }

    FORCE_INLINE Storage getStorage(uint32_t const input_offset, size_t const obj_align,
                                    size_t const obj_size) noexcept {
        auto const output_offset = cleanMemory();

        if (input_offset >= output_offset) {
            if (auto const storage = Storage::getAlignedStorage(m_Buffer + input_offset,
                                                                m_BufferSize - input_offset - 1, obj_align, obj_size)) {
                return storage;
            }

            if (output_offset) {
                if (auto const storage = Storage::getAlignedStorage(m_Buffer, output_offset - 1, obj_align, obj_size)) {
                    m_SentinelFollow = input_offset;
                    m_SentinelRead.store(input_offset, std::memory_order::relaxed);
                    return storage;
                }
            }
            return {};
        } else
            return Storage::getAlignedStorage(m_Buffer + input_offset, output_offset - input_offset - 1, obj_align,
                                              obj_size);
    }

    template<typename = void>
    requires(destroyNonInvoked) void destroyAllFO() {
        auto destroyAndGetNext = [this](uint32_t output_offset) noexcept -> uint32_t {
            auto const functionCxtPtr = align<FunctionContext>(m_Buffer + output_offset);
            auto const offset = functionCxtPtr->getNextAddr() - m_Buffer;
            functionCxtPtr->destroyFO();
            return offset;
        };

        auto const input_offset = m_InputOffset.load(std::memory_order::acquire).getValue();
        auto output_offset = cleanMemory();

        if (output_offset == input_offset) return;
        else if (output_offset > input_offset) {
            auto const sentinel = m_SentinelFollow;
            while (output_offset != sentinel) { output_offset = destroyAndGetNext(output_offset); }
            output_offset = 0;
        }

        while (output_offset != input_offset) { output_offset = destroyAndGetNext(output_offset); }
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

    std::atomic<TaggedUint32> m_InputOffset{};
    uint32_t m_OutputTailOffset{0};
    uint32_t m_SentinelFollow{NO_SENTINEL};

    std::atomic<TaggedUint32> m_OutputHeadOffset{};
    std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Empty> m_WriteFlag{};

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;
};

#endif
