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

namespace {
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
}// namespace

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
        using InvokeAndDestroy = R (*)(void *callable_ptr, Args...) noexcept;
        using Destroy = void (*)(void *callable_ptr) noexcept;

        struct ReadData {
            InvokeAndDestroy function;
            void *callable_ptr;
            std::byte *next_addr;
        };

        ReadData getReadData() const noexcept {
            return {std::bit_cast<InvokeAndDestroy>(getFp(fp_offset)),
                    std::bit_cast<std::byte *>(this) + callable_offset, getNextAddr()};
        }

        void destroyFO() const noexcept {
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

        explicit operator bool() const noexcept { return next_addr; }

        template<typename Callable, typename... CArgs>
        std::byte *construct(CArgs &&...args) const noexcept {
            uint16_t const callable_offset = callable_ptr - fc_ptr;
            uint16_t const stride = next_addr - fc_ptr;

            new (fc_ptr) FunctionContext{Type<Callable>{}, callable_offset, stride};
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
    FunctionQueue(std::byte *buffer, std::size_t size) noexcept
        : m_InputPos{buffer}, m_OutputPos{buffer}, m_Buffer{buffer}, m_BufferEnd{buffer + size} {}

    std::byte *buffer() const noexcept { return m_Buffer; }

    size_t buffer_size() const noexcept { return m_BufferEnd - m_Buffer; }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();

        m_InputPos = m_Buffer;
        m_OutputPos = m_Buffer;
        m_SentinelRead = nullptr;
    }

    ~FunctionQueue() noexcept {
        if constexpr (destroyNonInvoked) destroyAllFO();
    }

    bool empty() const noexcept { return m_InputPos == m_OutputPos; }

    FORCE_INLINE R call_and_pop(Args... args) const noexcept {
        auto const output_pos = m_OutputPos;
        bool const found_sentinel = output_pos == m_SentinelRead;

        auto const functionCxtPtr = std::bit_cast<FunctionContext *>(found_sentinel ? m_Buffer : output_pos);
        auto const read_data = functionCxtPtr->getReadData();

        ScopeGaurd const update_state{[&] {
            if (found_sentinel) m_SentinelRead = nullptr;
            m_OutputPos = read_data.next_addr;
        }};

        return read_data.function(read_data.callable_ptr, args...);
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
        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const storage = getStorage<callable_align, callable_size>();
        if (!storage) return false;

        auto const next_addr = storage.template construct<Callable>(std::forward<CArgs>(args)...);
        m_InputPos = next_addr;

        return true;
    }

    static constexpr inline size_t BUFFER_ALIGNMENT = alignof(FunctionContext);

private:
    template<typename T, size_t alignment = alignof(T)>
    static constexpr T *align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + alignment) & -alignment);
    }

    template<typename Callable>
    static R invokeAndDestroy(void *data, Args... args) noexcept {
        auto const functor_ptr = static_cast<Callable *>(data);
        ScopeGaurd const destroy_functor{[&] { std::destroy_at(functor_ptr); }};

        return (*functor_ptr)(std::forward<Args>(args)...);
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
    }

    void destroyAllFO() {
        auto const input_pos = m_InputPos;
        auto output_pos = m_OutputPos;

        auto destroyAndGetNextPos = [](auto pos) {
            auto const functionCxtPtr = std::bit_cast<FunctionContext *>(pos);
            functionCxtPtr->destroyFO();
            return functionCxtPtr->getNextAddr();
        };

        if (input_pos == output_pos) return;
        else if (output_pos > input_pos) {
            auto const sentinel = m_SentinelRead;
            while (output_pos != sentinel) { output_pos = destroyAndGetNextPos(output_pos); }
            output_pos = m_Buffer;
        }

        while (output_pos != input_pos) { output_pos = destroyAndGetNextPos(output_pos); }
    }

    template<size_t obj_align, size_t obj_size>
    FORCE_INLINE Storage getStorage() noexcept {
        auto const input_pos = m_InputPos;
        auto const output_pos = m_OutputPos;
        auto const buffer_start = m_Buffer;

        constexpr auto getAlignedStorage = Storage::template getAlignedStorage<obj_align, obj_size>;

        if (input_pos >= output_pos) {
            if (auto const storage = getAlignedStorage(input_pos, m_BufferEnd)) {
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
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    std::byte *m_InputPos;
    mutable std::byte *m_OutputPos;
    mutable std::byte *m_SentinelRead{};

    std::byte *const m_Buffer;
    std::byte *const m_BufferEnd;
};

#endif
