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

template<typename T, bool isWriteProtected, bool destroyNonInvoked = true,
         size_t max_obj_footprint = alignof(std::max_align_t) + 128>
class FunctionQueue_MCSP {};

template<typename R, typename... Args, bool isWriteProtected, bool destroyNonInvoked, size_t max_obj_footprint>
class FunctionQueue_MCSP<R(Args...), isWriteProtected, destroyNonInvoked, max_obj_footprint> {
private:
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
            auto const function = getFp<InvokeAndDestroy>(fp_offset);
            return {function, this_ + callable_offset};
        }

        std::byte *destroyFO() const noexcept {
            auto const this_ = std::bit_cast<std::byte *>(this);
            auto const destroy_functor = getFp<Destroy>(destroyFp_offset);

            destroy_functor(this_ + callable_offset);
            return this_ + stride;
        }

        uint16_t getStride() const noexcept { return stride; }

        void free(std::atomic<uint16_t> *strideArray) noexcept {
            strideArray[strideArrayIndex].store(stride, std::memory_order::release);
        }

    private:
        template<typename Callable>
        FunctionContext(rb_detail::Type<Callable>, uint16_t callable_offset, uint16_t stride,
                        uint32_t strideArrayIndex) noexcept
            : fp_offset{getFpOffset(&invokeAndDestroy<Callable>)}, destroyFp_offset{getFpOffset(&destroy<Callable>)},
              callable_offset{callable_offset}, stride{stride}, strideArrayIndex{strideArrayIndex} {}

        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, rb_detail::Empty> destroyFp_offset;
        uint16_t const callable_offset;
        uint16_t const stride;
        uint32_t const strideArrayIndex;

        friend class Storage;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return fc_ptr; }

        std::byte *getNextAddr() const noexcept { return next_addr; }

        template<typename Callable, typename... CArgs>
        void construct(rb_detail::Type<Callable>, CArgs &&...args) const noexcept {
            uint16_t const callable_offset = callable_ptr - fc_ptr;
            uint16_t const stride = next_addr - fc_ptr;

            new (fc_ptr) FunctionContext{rb_detail::Type<Callable>{}, callable_offset, stride, strideArrayIndex};
            new (callable_ptr) Callable{std::forward<CArgs>(args)...};
        }

        template<size_t obj_align, size_t obj_size>
        static Storage getAlignedStorage(std::byte *buffer_start, uint32_t strideArrayIndex) noexcept {
            auto const fc_ptr = buffer_start;
            auto const obj_ptr = align<std::byte, obj_align>(fc_ptr + sizeof(FunctionContext));
            auto const next_addr = align<std::byte, alignof(FunctionContext)>(obj_ptr + obj_size);
            return {fc_ptr, obj_ptr, next_addr, strideArrayIndex};
        }

    private:
        Storage(std::byte *fc_ptr, std::byte *callable_ptr, std::byte *next_addr, uint32_t strideArrayIndex) noexcept
            : fc_ptr{fc_ptr}, callable_ptr{callable_ptr}, next_addr{next_addr}, strideArrayIndex{strideArrayIndex} {}

        std::byte *fc_ptr{};
        std::byte *callable_ptr;
        std::byte *next_addr;
        uint32_t strideArrayIndex;
    };

    static constexpr inline size_t function_context_footprint = alignof(FunctionContext) + sizeof(FunctionContext);
    static constexpr inline size_t sentinel_region_size = function_context_footprint + max_obj_footprint;

public:
    static constexpr inline size_t BUFFER_ALIGNMENT = alignof(FunctionContext);

    static_assert(sentinel_region_size <= std::numeric_limits<uint16_t>::max());

    template<typename Callable, typename... CArgs>
    static constexpr bool is_valid_callable_v =
            std::is_nothrow_constructible_v<Callable, CArgs...> &&std::is_nothrow_destructible_v<Callable> &&
                    std::is_nothrow_invocable_r_v<R, Callable, Args...> &&
            ((alignof(Callable) + sizeof(Callable) + function_context_footprint) <= sentinel_region_size);

    class FunctionHandle {
    public:
        FunctionHandle() noexcept = default;

        FunctionHandle(FunctionHandle &&other) noexcept
            : functionCxtPtr{std::exchange(other.functionCxtPtr, nullptr)}, strideArray{other.strideArray} {}

        FunctionHandle(FunctionHandle const &) = delete;
        FunctionHandle &operator=(FunctionHandle const &other) = delete;

        operator bool() const noexcept { return functionCxtPtr; }

        R call_and_pop(Args... args) noexcept {
            rb_detail::ScopeGaurd const free_function_cxt = {[&] {
                functionCxtPtr->free(strideArray);
                functionCxtPtr = nullptr;
            }};

            auto const read_data = functionCxtPtr->getReadData();
            return read_data.function(read_data.callable_ptr, std::forward<Args>(args)...);
        }

        FunctionHandle &operator=(FunctionHandle &&other) noexcept {
            if (functionCxtPtr) release();
            functionCxtPtr = std::exchange(other.functionCxtPtr, nullptr);
            strideArray = other.strideArray;
            return *this;
        }

        ~FunctionHandle() {
            if (functionCxtPtr) release();
        }

    private:
        void release() {
            if constexpr (destroyNonInvoked) functionCxtPtr->destroyFO();
            functionCxtPtr->free(strideArray);
        }

        friend class FunctionQueue_MCSP;

        explicit FunctionHandle(FunctionContext *fcp, std::atomic<uint16_t> *strideArray) noexcept
            : functionCxtPtr{fcp}, strideArray{strideArray} {}

        FunctionContext *functionCxtPtr{};
        std::atomic<uint16_t> *strideArray;
    };

    static constexpr uint32_t clean_array_size(size_t buffer_size) noexcept {
        return buffer_size / sizeof(FunctionContext) + 1;
    }

    FunctionQueue_MCSP(std::byte *buffer, size_t size, std::atomic<uint16_t> *cleanOffsetArray) noexcept
        : m_Buffer{buffer}, m_StrideArray{cleanOffsetArray},
          m_BufferEndOffset{static_cast<uint32_t>(size - sentinel_region_size)}, m_StrideArraySize{
                                                                                         clean_array_size(size)} {
        resetStrideArray();
    }

    ~FunctionQueue_MCSP() noexcept {
        if constexpr (destroyNonInvoked) {
            if constexpr (isWriteProtected)
                while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                    ;

            destroyAllFO();
        }
    }

    std::byte *buffer() const noexcept { return m_Buffer; }

    size_t buffer_size() const noexcept { return m_BufferEndOffset + sentinel_region_size; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire) == m_OutputReadOffset.load(std::memory_order::relaxed);
    }

    void clear() noexcept {
        if constexpr (isWriteProtected)
            while (m_WriteFlag.test_and_set(std::memory_order::acquire))
                ;

        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        if constexpr (destroyNonInvoked) destroyAllFO();
        resetStrideArray();

        m_InputOffset.store({}, std::memory_order::relaxed);
        m_StrideHeadIndex = 0;
        m_StrideTailIndex = 0;
        m_OutputFollowOffset = 0;
        m_OutputReadOffset.store({}, std::memory_order::relaxed);
    }

    FunctionHandle get_function_handle() const noexcept { return FunctionHandle{getFunctionContext(), m_StrideArray}; }

    FunctionHandle get_function_handle_check_once() const noexcept {
        return FunctionHandle{getFunctionContextCheckOnce(), m_StrideArray};
    }

    template<auto invokable>
    requires std::is_nothrow_invocable_r_v<R, decltype(invokable), Args...>
    bool push() noexcept {
        return push([](Args... args) noexcept {
            if constexpr (std::is_function_v<std::remove_pointer_t<decltype(invokable)>>)
                return invokable(std::forward<Args>(args)...);
            else
                return std::invoke(invokable, std::forward<Args>(args)...);
        });
    }

    template<typename T>
    bool push(T &&callable) noexcept {
        using Callable = std::decay_t<T>;
        return emplace<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    requires is_valid_callable_v<Callable, CArgs...>
    bool emplace(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected)
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return false;

        rb_detail::ScopeGaurd const release_write_lock{[&] {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
        }};

        cleanMemory();

        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const storage = getStorage<callable_align, callable_size>(input_offset.getValue());
        if (!storage) return false;

        storage.construct(rb_detail::Type<Callable>{}, std::forward<CArgs>(args)...);

        uint32_t const next_offset = storage.getNextAddr() - m_Buffer;
        auto const nextInputOffset = input_offset.getIncrTagged(next_offset < m_BufferEndOffset ? next_offset : 0);

        m_InputOffset.store(nextInputOffset, std::memory_order::release);

        ++m_StrideHeadIndex;
        if (m_StrideHeadIndex == m_StrideArraySize) m_StrideHeadIndex = 0;

        if (nextInputOffset.getTag() == 0) {
            auto output_offset = nextInputOffset;
            while (!m_OutputReadOffset.compare_exchange_weak(output_offset,
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
        FunctionContext *fcxt_ptr;
        rb_detail::TaggedUint32 next_output_offset;

        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);
        auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);

        do {
            if (input_offset.getTag() < output_offset.getTag() || input_offset.getValue() == output_offset.getValue())
                return nullptr;
            fcxt_ptr = std::bit_cast<FunctionContext *>(m_Buffer + output_offset.getValue());
            auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
            next_output_offset = input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);
        } while (!m_OutputReadOffset.compare_exchange_weak(output_offset, next_output_offset,
                                                           std::memory_order::relaxed, std::memory_order::relaxed));
        return fcxt_ptr;
    }

    FunctionContext *getFunctionContextCheckOnce() const noexcept {
        auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

        if (input_offset.getTag() < output_offset.getTag() || input_offset.getValue() == output_offset.getValue())
            return nullptr;

        auto const fcxt_ptr = std::bit_cast<FunctionContext *>(m_Buffer + output_offset.getValue());
        auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
        auto const next_output_offset = input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);

        if (m_OutputReadOffset.compare_exchange_strong(output_offset, next_output_offset, std::memory_order::relaxed,
                                                       std::memory_order::relaxed))
            return fcxt_ptr;
        else
            return nullptr;
    }

    void cleanMemory() noexcept {
        auto output_offset = m_OutputFollowOffset;
        auto clean_range = [&output_offset, stride_array = m_StrideArray,
                            buffer_end_offset = m_BufferEndOffset](uint32_t index, uint32_t end) {
            for (; index != end; ++index)
                if (auto const stride = stride_array[index].load(std::memory_order::acquire); stride) {
                    output_offset += stride;
                    if (output_offset >= buffer_end_offset) output_offset = 0;
                    stride_array[index].store(0, std::memory_order::relaxed);
                } else
                    break;

            return index;
        };

        if (m_StrideTailIndex == m_StrideHeadIndex) return;
        else if (m_StrideTailIndex > m_StrideHeadIndex) {
            m_StrideTailIndex = clean_range(m_StrideTailIndex, m_StrideArraySize);
            m_OutputFollowOffset = output_offset;
            if (m_StrideTailIndex != m_StrideArraySize) return;
            else
                m_StrideTailIndex = 0;
        }

        m_StrideTailIndex = clean_range(m_StrideTailIndex, m_StrideHeadIndex);
        m_OutputFollowOffset = output_offset;
    }

    void resetStrideArray() noexcept {
        for (uint32_t i = 0; i != m_StrideArraySize; ++i) m_StrideArray[i].store(0, std::memory_order::relaxed);
    }

    void destroyAllFO() {
        FunctionContext *fcxt_ptr;
        while ((fcxt_ptr = getFunctionContextCheckOnce())) fcxt_ptr->destroyFO();
    }

    template<size_t obj_align, size_t obj_size>
    Storage getStorage(uint32_t input_offset) const noexcept {
        auto const input_pos = m_Buffer + input_offset;
        auto const output_pos = m_Buffer + m_OutputFollowOffset;
        auto const buffer_start = m_Buffer;
        auto const buffer_end = m_Buffer + m_BufferEndOffset;
        auto const strideArrayIndex = m_StrideHeadIndex;

        constexpr auto getAlignedStorage = Storage::template getAlignedStorage<obj_align, obj_size>;
        if (auto const storage = getAlignedStorage(input_pos, strideArrayIndex);
            (storage.getNextAddr() < output_pos) ||
            ((input_pos >= output_pos) && ((storage.getNextAddr() < buffer_end) || (output_pos - buffer_start))))
            return storage;

        return {};
    }

    static void fp_base_func() noexcept {}

    template<typename FPtr>
    requires std::is_function_v<std::remove_pointer_t<FPtr>>
    static auto getFp(uint32_t fp_offset) noexcept {
        const uintptr_t fp_base =
                std::bit_cast<uintptr_t>(&fp_base_func) & static_cast<uintptr_t>(0XFFFFFFFF00000000lu);
        return std::bit_cast<FPtr>(fp_base + fp_offset);
    }

    template<typename FPtr>
    requires std::is_function_v<std::remove_pointer_t<FPtr>>
    static uint32_t getFpOffset(FPtr fp) noexcept { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(fp)); }

private:
    static constexpr uint32_t INVALID_INDEX{std::numeric_limits<uint32_t>::max()};

    std::atomic<rb_detail::TaggedUint32> m_InputOffset{};
    uint32_t m_OutputFollowOffset{};
    uint32_t m_StrideHeadIndex{};
    uint32_t m_StrideTailIndex{};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, rb_detail::Empty> m_WriteFlag{};

    mutable std::atomic<rb_detail::TaggedUint32> m_OutputReadOffset{};

    std::byte *const m_Buffer;
    std::atomic<uint16_t> *const m_StrideArray;
    uint32_t const m_BufferEndOffset;
    uint32_t const m_StrideArraySize;
};

#endif
