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

namespace fq_detail {
    template<typename Function>
    class ScopeGaurd {
    public:
        template<typename F>
        ScopeGaurd(F &&func) : m_Func{std::forward<F>(func)} {}

        void commit() noexcept { m_Commit = true; }

        ~ScopeGaurd() {
            if (!m_Commit) m_Func();
        }

    private:
        Function m_Func;
        bool m_Commit{false};
    };

    template<typename Func>
    ScopeGaurd(Func &&) -> ScopeGaurd<std::decay_t<Func>>;


    class Empty {
    public:
        template<typename... T>
        explicit Empty(T &&...) noexcept {}
    };

    template<typename>
    class Type {};

}// namespace fq_detail

template<typename T, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked = true>
class FunctionQueue_SCSP {};

template<typename R, typename... Args, bool isReadProtected, bool isWriteProtected, bool destroyNonInvoked>
class FunctionQueue_SCSP<R(Args...), isReadProtected, isWriteProtected, destroyNonInvoked> {
private:
    class Storage;

    class FunctionContext {
    public:
        using InvokeAndDestroy = R (*)(void *callable_ptr, Args...) noexcept;
        using Destroy = void (*)(void *callable_ptr) noexcept;

        struct ReadData {
            InvokeAndDestroy function;
            void *callable_ptr;
            std::byte *next_addr;
        };

        ReadData getReadData() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const function = std::bit_cast<InvokeAndDestroy>(getFp(fp_offset));
            return {function, this_ + callable_offset, this_ + stride};
        }

        std::byte *destroyFO() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const destroy_functor = std::bit_cast<Destroy>(getFp(destroyFp_offset));

            destroy_functor(this_ + callable_offset);
            return this_ + stride;
        }

    private:
        template<typename Callable>
        FunctionContext(fq_detail::Type<Callable>, uint16_t callable_offset, uint16_t stride) noexcept
            : fp_offset{getFpOffset(&invokeAndDestroy<Callable>)}, destroyFp_offset{getFpOffset(&destroy<Callable>)},
              callable_offset{callable_offset}, stride{stride} {}

        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, fq_detail::Empty> destroyFp_offset;
        uint16_t const callable_offset;
        uint16_t const stride;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return next_addr; }

        template<typename Callable, typename... CArgs>
        std::byte *construct(CArgs &&...args) const noexcept {
            uint16_t const callable_offset = callable_ptr - fc_ptr;
            uint16_t const stride = next_addr - fc_ptr;

            new (fc_ptr) FunctionContext{fq_detail::Type<Callable>{}, callable_offset, stride};
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
    FunctionQueue_SCSP(std::byte *buffer, size_t size) noexcept
        : m_InputPos{buffer}, m_OutputPos{buffer}, m_Buffer{buffer}, m_BufferEnd{buffer + size} {}

    ~FunctionQueue_SCSP() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();
    }

    std::byte *buffer() const noexcept { return m_Buffer; }

    size_t buffer_size() const noexcept { return m_BufferEnd - m_Buffer; }

    bool empty() const noexcept {
        return m_InputPos.load(std::memory_order::acquire) == m_OutputPos.load(std::memory_order::relaxed);
    }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputPos.store(m_Buffer, std::memory_order::relaxed);
        m_OutputPos.store(m_Buffer, std::memory_order::relaxed);
        m_SentinelRead.store(nullptr, std::memory_order::relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::relaxed);
    }

    bool reserve() const noexcept {
        if constexpr (isReadProtected)
            if (m_ReadFlag.test_and_set(std::memory_order::acquire)) return false;

        fq_detail::ScopeGaurd release_read_lock{[&] {
            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::relaxed);
        }};

        bool const has_functions = !empty();
        if (has_functions) release_read_lock.commit();
        return has_functions;
    }

    R call_and_pop(Args... args) const noexcept {
        auto const output_pos = m_OutputPos.load(std::memory_order::relaxed);
        bool const found_sentinel = output_pos == m_SentinelRead.load(std::memory_order::relaxed);

        auto const functionCxtPtr = std::bit_cast<FunctionContext *>(found_sentinel ? m_Buffer : output_pos);
        auto const read_data = functionCxtPtr->getReadData();

        fq_detail::ScopeGaurd const update_state{[&] {
            if (found_sentinel) m_SentinelRead.store(nullptr, std::memory_order::relaxed);
            m_OutputPos.store(read_data.next_addr, std::memory_order::release);
            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);
        }};

        return read_data.function(read_data.callable_ptr, std::forward<Args>(args)...);
    }

    template<R (*function)(Args...)>
    bool push_back() noexcept {
        return push_back([](Args... args) { return function(std::forward<Args>(args)...); });
    }

    template<typename T>
    bool push_back(T &&callable) noexcept {
        using Callable = std::decay_t<T>;
        return emplace_back<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    bool emplace_back(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;
        }

        fq_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const storage = getStorage<callable_align, callable_size>();
        if (!storage) return false;

        auto const next_addr = storage.template construct<Callable>(std::forward<CArgs>(args)...);
        m_InputPos.store(next_addr, std::memory_order::release);

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
        fq_detail::ScopeGaurd const destroy_functor{[&] { std::destroy_at(functor_ptr); }};

        return (*functor_ptr)(std::forward<Args>(args)...);
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
    }

    void destroyAllFO() {
        auto destroyAndGetNextPos = [](auto pos) {
            auto const functionCxtPtr = std::bit_cast<FunctionContext *>(pos);
            return functionCxtPtr->destroyFO();
        };

        auto const input_pos = m_InputPos.load(std::memory_order::relaxed);
        auto output_pos = m_OutputPos.load(std::memory_order::acquire);

        if (input_pos == output_pos) return;
        else if (output_pos > input_pos) {
            auto const sentinel = m_SentinelRead.load(std::memory_order::relaxed);
            while (output_pos != sentinel) { output_pos = destroyAndGetNextPos(output_pos); }
            output_pos = m_Buffer;
        }

        while (output_pos != input_pos) { output_pos = destroyAndGetNextPos(output_pos); }
    }

    template<size_t obj_align, size_t obj_size>
    Storage getStorage() noexcept {
        auto const input_pos = m_InputPos.load(std::memory_order::relaxed);
        auto const output_pos = m_OutputPos.load(std::memory_order::acquire);
        auto const buffer_start = m_Buffer;
        auto const buffer_end = m_BufferEnd;

        constexpr auto getAlignedStorage = Storage::template getAlignedStorage<obj_align, obj_size>;

        if (input_pos >= output_pos) {
            if (auto const storage = getAlignedStorage(input_pos, buffer_end)) {
                return storage;
            } else if (auto const storage = getAlignedStorage(buffer_start, output_pos)) {
                m_SentinelRead = input_pos;
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
    std::atomic<std::byte *> m_InputPos;
    mutable std::atomic<std::byte *> m_OutputPos;
    mutable std::atomic<std::byte *> m_SentinelRead{};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, fq_detail::Empty> m_WriteFlag{};
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, fq_detail::Empty> m_ReadFlag{};

    std::byte *const m_Buffer;
    std::byte *const m_BufferEnd;
};

#endif
