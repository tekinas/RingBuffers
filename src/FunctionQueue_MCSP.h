#ifndef FUNCTIONQUEUE_MCSP
#define FUNCTIONQUEUE_MCSP

#include "rb_detail.h"
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

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

    class Storage;

    class FunctionContext {
    public:
        using InvokeAndDestroy = R (*)(void *callable_ptr, Args...) noexcept;
        using Destroy = void (*)(void *callable_ptr) noexcept;

        struct ReadData {
            InvokeAndDestroy function;
            void *callable_ptr;
        };

        ReadData getReadData() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const function = std::bit_cast<InvokeAndDestroy>(getFp(fp_offset.load(std::memory_order::relaxed)));
            return {function, this_ + callable_offset};
        }

        std::byte *destroyFO() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const destroy_functor = std::bit_cast<Destroy>(getFp(destroyFp_offset));

            destroy_functor(this_ + callable_offset);
            return this_ + stride;
        }

        std::byte *getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + stride; }

        void free() noexcept { fp_offset.store(0, std::memory_order::release); }

        [[nodiscard]] bool isFree() const noexcept { return fp_offset.load(std::memory_order::acquire) == 0; }

    private:
        template<typename Callable>
        FunctionContext(rb_detail::Type<Callable>, uint16_t callable_offset, uint16_t stride) noexcept
            : fp_offset{getFpOffset(&invokeAndDestroy<Callable>)}, destroyFp_offset{getFpOffset(&destroy<Callable>)},
              callable_offset{callable_offset}, stride{stride} {}

        std::atomic<uint32_t> fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, rb_detail::Empty> destroyFp_offset;
        uint16_t const callable_offset;
        uint16_t const stride;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return next_addr; }

        template<typename Callable, typename... CArgs>
        std::byte *construct(rb_detail::Type<Callable>, CArgs &&...args) const noexcept {
            uint16_t const callable_offset = callable_ptr - fc_ptr;
            uint16_t const stride = next_addr - fc_ptr;

            new (fc_ptr) FunctionContext{rb_detail::Type<Callable>{}, callable_offset, stride};
            new (callable_ptr) Callable{std::forward<CArgs>(args)...};

            return next_addr;
        }

        template<size_t obj_align, size_t obj_size>
        static Storage getAlignedStorage(std::byte *buffer_start, std::byte *buffer_end) noexcept {
            auto const fc_ptr = buffer_start;
            auto const obj_ptr = align<std::byte, obj_align>(fc_ptr + sizeof(FunctionContext));
            auto const next_addr = align<std::byte, alignof(FunctionContext)>(obj_ptr + obj_size);
            return {fc_ptr, obj_ptr, next_addr < buffer_end ? next_addr : nullptr};
        }

    private:
        Storage(std::byte *fc_ptr, std::byte *callable_ptr, std::byte *next_addr) noexcept
            : fc_ptr{fc_ptr}, callable_ptr{callable_ptr}, next_addr{next_addr} {}

        std::byte *const fc_ptr{};
        std::byte *const callable_ptr{};
        std::byte *const next_addr{};
    };

public:
    static constexpr inline size_t BUFFER_ALIGNMENT = alignof(FunctionContext);

    template<typename Callable, typename... CArgs>
    static constexpr bool is_valid_callable_v =
            std::is_nothrow_constructible_v<Callable, CArgs...> &&std::is_nothrow_destructible_v<Callable> &&
                    std::is_nothrow_invocable_r_v<R, Callable, Args...> &&
            ((alignof(FunctionContext) + sizeof(FunctionContext) + alignof(Callable) + sizeof(Callable)) <=
             std::numeric_limits<uint16_t>::max());

    class FunctionHandle {
    public:
        FunctionHandle() noexcept = default;

        FunctionHandle(FunctionHandle &&other) noexcept
            : functionCxtPtr{std::exchange(other.functionCxtPtr, nullptr)} {}

        FunctionHandle(FunctionHandle const &) = delete;

        operator bool() const noexcept { return functionCxtPtr; }

        R call_and_pop(Args... args) noexcept {
            rb_detail::ScopeGaurd const clean_up = {[&] {
                functionCxtPtr->free();
                functionCxtPtr = nullptr;
            }};

            auto const read_data = functionCxtPtr->getReadData();
            return read_data.function(read_data.callable_ptr, std::forward<Args>(args)...);
        }

        FunctionHandle &operator=(FunctionHandle &&other) noexcept {
            release();
            functionCxtPtr = std::exchange(other.functionCxtPtr, nullptr);
            return *this;
        }

        ~FunctionHandle() {
            if (functionCxtPtr) release();
        }

    private:
        void release() {
            if constexpr (destroyNonInvoked) functionCxtPtr->destroyFO();
            functionCxtPtr->free();
        }

        friend class FunctionQueue_MCSP;

        explicit FunctionHandle(FunctionContext *fcp) noexcept : functionCxtPtr{fcp} {}

        FunctionContext *functionCxtPtr{};
    };

    FunctionQueue_MCSP(std::byte *buffer, size_t size) noexcept
        : m_OutputTailPos{buffer}, m_Buffer{buffer}, m_BufferEnd{buffer + size} {}

    ~FunctionQueue_MCSP() noexcept {
        if constexpr (destroyNonInvoked) {
            if constexpr (isWriteProtected)
                while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                    ;
            consumeAllFO();
        }
    }

    std::byte *buffer() const noexcept { return m_Buffer; }

    size_t buffer_size() const noexcept { return m_BufferEnd - m_Buffer; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire) == m_OutputHeadOffset.load(std::memory_order::relaxed);
    }

    void clear() noexcept {
        if constexpr (isWriteProtected)
            while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                ;
        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        consumeAllFO();

        m_InputOffset.store({}, std::memory_order::relaxed);
        m_OutputTailPos = m_Buffer;
        m_SentinelFollow = nullptr;

        m_OutputHeadOffset.store({}, std::memory_order::relaxed);
        m_SentinelRead.store(nullptr, std::memory_order::relaxed);
    }

    FunctionHandle get_function_handle() const noexcept { return FunctionHandle{getFunctionContext()}; }

    template<auto invokable>
    requires std::is_nothrow_invocable_r_v<R, decltype(invokable), Args...>
    bool push_back() noexcept {
        return push_back([](Args... args) noexcept {
            if constexpr (std::is_function_v<std::remove_pointer_t<decltype(invokable)>>)
                return invokable(std::forward<Args>(args)...);
            else
                return std::invoke(invokable, std::forward<Args>(args)...);
        });
    }

    template<typename T>
    bool push_back(T &&callable) noexcept {
        using Callable = std::decay_t<T>;
        return emplace_back<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    requires is_valid_callable_v<Callable, CArgs...>
    bool emplace_back(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected)
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;

        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const storage = getStorage<callable_align, callable_size>(input_offset.getValue());
        if (!storage) return false;

        auto const nextInputOffsetValue =
                storage.construct(rb_detail::Type<Callable>{}, std::forward<CArgs>(args)...) - m_Buffer;
        auto const nextInputOffset = input_offset.getIncrTagged(nextInputOffsetValue);

        m_InputOffset.store(nextInputOffset, std::memory_order::release);

        if (nextInputOffset.getTag() == 0) {
            auto output_offset = nextInputOffset;
            while (!m_OutputHeadOffset.compare_exchange_weak(output_offset,
                                                             nextInputOffset.getSameTagged(output_offset.getValue()),
                                                             std::memory_order::relaxed, std::memory_order::relaxed))
                ;
        }

        return true;
    }

private:
    template<typename T, size_t alignment = alignof(T)>
    static constexpr T *align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + alignment) & -alignment);
    }

    template<typename Callable>
    static R invokeAndDestroy(void *data, Args... args) noexcept {
        auto const functor_ptr = static_cast<Callable *>(data);
        rb_detail::ScopeGaurd const destroy_functor{[&] { std::destroy_at(functor_ptr); }};

        return std::invoke(*functor_ptr, std::forward<Args>(args)...);
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
    }

    FunctionContext *getFunctionContext() const noexcept {
        bool found_sentinel = false;
        FunctionContext *fcxt_ptr;
        TaggedUint32 next_output_offset;

        rb_detail::ScopeGaurd const reset_sentinel{[&] {
            if (found_sentinel) m_SentinelRead.store(nullptr, std::memory_order::relaxed);
        }};

        auto output_offset = m_OutputHeadOffset.load(std::memory_order::relaxed);

        do {
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);
            auto const output_pos = m_Buffer + output_offset.getValue();

            if ((input_offset.getTag() < output_offset.getTag()) |
                (input_offset.getValue() == output_offset.getValue()))
                return nullptr;

            found_sentinel = output_pos == m_SentinelRead.load(std::memory_order::relaxed);
            fcxt_ptr = std::bit_cast<FunctionContext *>(found_sentinel ? m_Buffer : output_pos);

            next_output_offset = input_offset.getSameTagged(fcxt_ptr->getNextAddr() - m_Buffer);

        } while (!m_OutputHeadOffset.compare_exchange_weak(output_offset, next_output_offset,
                                                           std::memory_order::relaxed, std::memory_order::relaxed));

        return fcxt_ptr;
    }

    std::byte *cleanMemory() noexcept {
        auto const output_head = m_Buffer + m_OutputHeadOffset.load(std::memory_order::acquire).getValue();
        auto output_tail = m_OutputTailPos;

        if (output_tail != output_head) {
            while (output_tail != output_head) {
                if (output_tail == m_SentinelFollow) {
                    output_tail = m_Buffer;
                    m_SentinelFollow = nullptr;
                } else if (auto const functionCxt = std::bit_cast<FunctionContext *>(output_tail);
                           functionCxt->isFree()) {
                    output_tail = functionCxt->getNextAddr();
                } else
                    break;
            }

            m_OutputTailPos = output_tail;
        }

        return output_tail;
    }

    void consumeAllFO() {
        for (auto fcxt_ptr = getFunctionContext(); fcxt_ptr;)
            if constexpr (destroyNonInvoked) fcxt_ptr->destroyFO();
    }

    template<size_t obj_align, size_t obj_size>
    Storage getStorage(uint32_t input_offset) noexcept {
        auto const input_pos = m_Buffer + input_offset;
        auto const output_pos = cleanMemory();
        auto const buffer_start = m_Buffer;
        auto const buffer_end = m_BufferEnd;

        constexpr auto getAlignedStorage = Storage::template getAlignedStorage<obj_align, obj_size>;

        if (input_pos >= output_pos) {
            if (auto const storage = getAlignedStorage(input_pos, buffer_end)) {
                return storage;
            } else if (auto const storage = getAlignedStorage(buffer_start, output_pos)) {
                m_SentinelRead.store(input_pos, std::memory_order::relaxed);
                m_SentinelFollow = input_pos;
                return storage;
            } else
                return {};
        } else
            return getAlignedStorage(input_pos, output_pos);
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
    static constexpr uint32_t INVALID_INDEX{std::numeric_limits<uint32_t>::max()};

    std::atomic<TaggedUint32> m_InputOffset{};
    std::byte *m_OutputTailPos;
    std::byte *m_SentinelFollow{};

    mutable std::atomic<TaggedUint32> m_OutputHeadOffset{};
    mutable std::atomic<std::byte *> m_SentinelRead{};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, rb_detail::Empty> m_WriteFlag{};

    std::byte *const m_Buffer;
    std::byte *const m_BufferEnd;
};

#endif
