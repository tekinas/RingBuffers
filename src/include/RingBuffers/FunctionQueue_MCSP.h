#ifndef FUNCTIONQUEUE_MCSP
#define FUNCTIONQUEUE_MCSP

#include "detail/rb_detail.h"
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

template<typename T, size_t max_reader_threads, bool destroyNonInvoked = true,
         size_t max_obj_footprint = alignof(std::max_align_t) + 128>
class FunctionQueue_MCSP {};

template<typename R, typename... Args, size_t max_reader_threads, bool destroyNonInvoked, size_t max_obj_footprint>
class FunctionQueue_MCSP<R(Args...), max_reader_threads, destroyNonInvoked, max_obj_footprint> {
private:
    class FunctionContext {
    public:
        R operator()(Args... args) const noexcept {
            return m_InvokeAndDestroy(getCallableAddr(), static_cast<Args>(args)...);
        }

        void destroyFO() const noexcept { m_Destroy(getCallableAddr()); }

        uint16_t getStride() const noexcept { return stride; }

        template<typename Callable>
        FunctionContext(rb::detail::type_tag<Callable>, uint16_t callable_offset, uint16_t stride) noexcept
            : m_InvokeAndDestroy{invokeAndDestroy<Callable>}, m_Destroy{destroy<Callable>},
              callable_offset{callable_offset}, stride{stride} {}

    private:
        void *getCallableAddr() const noexcept { return std::bit_cast<std::byte *>(this) + callable_offset; }

        rb::detail::FunctionPtr<R(void *, Args...)> m_InvokeAndDestroy;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, rb::detail::FunctionPtr<void(void *)>,
                                                 rb::detail::Empty>
                m_Destroy;
        uint16_t const callable_offset;
        uint16_t const stride;
    };

    class Storage {
    public:
        Storage() noexcept = default;

        explicit operator bool() const noexcept { return fc_ptr; }

        std::byte *getNextAddr() const noexcept { return next_addr; }

        template<typename Callable, typename... CArgs>
        void construct(rb::detail::type_tag<Callable>, CArgs &&...args) const noexcept {
            auto const callable_offset = static_cast<uint16_t>(callable_ptr - fc_ptr);
            auto const stride = static_cast<uint16_t>(next_addr - fc_ptr);

            new (fc_ptr) FunctionContext{rb::detail::type<Callable>, callable_offset, stride};
            new (callable_ptr) Callable{std::forward<CArgs>(args)...};
        }

        template<size_t obj_align, size_t obj_size>
        static Storage getAlignedStorage(std::byte *buffer_start) noexcept {
            auto const fc_ptr = buffer_start;
            auto const obj_ptr = rb::detail::align<std::byte, obj_align>(fc_ptr + sizeof(FunctionContext));
            auto const next_addr = rb::detail::align<std::byte, alignof(FunctionContext)>(obj_ptr + obj_size);
            return {fc_ptr, obj_ptr, next_addr};
        }

    private:
        Storage(std::byte *fc_ptr, std::byte *callable_ptr, std::byte *next_addr) noexcept
            : fc_ptr{fc_ptr}, callable_ptr{callable_ptr}, next_addr{next_addr} {}

        std::byte *fc_ptr{};
        std::byte *callable_ptr;
        std::byte *next_addr;
    };

    using CxtPtr = std::pair<FunctionContext *, std::byte *>;
    using ReaderPos = std::atomic<std::byte *>;
    using ReaderPosPtrArray = std::array<ReaderPos *, max_reader_threads>;
    using CacheLine = std::aligned_storage_t<64>;

    static constexpr size_t function_context_footprint = alignof(FunctionContext) + sizeof(FunctionContext);
    static constexpr size_t sentinel_region_size = function_context_footprint + max_obj_footprint;
    static constexpr size_t buffer_alignment = alignof(FunctionContext);
    static_assert(sentinel_region_size <= std::numeric_limits<uint16_t>::max());

    template<typename Callable, typename... CArgs>
    static constexpr bool is_valid_callable_v =
            std::is_nothrow_constructible_v<Callable, CArgs...> &&std::is_nothrow_destructible_v<Callable> &&
                    std::is_nothrow_invocable_r_v<R, Callable, Args...> &&
            ((alignof(Callable) + sizeof(Callable) + function_context_footprint) <= sentinel_region_size);

public:
    using allocator_type = std::pmr::polymorphic_allocator<>;

    class FunctionHandle {
    public:
        FunctionHandle() noexcept = default;

        FunctionHandle(FunctionHandle &&other) noexcept
            : cxt_ptr{std::exchange(other.cxt_ptr, {nullptr, nullptr})}, reader_pos{other.reader_pos} {}

        FunctionHandle(FunctionHandle const &) = delete;

        FunctionHandle &operator=(FunctionHandle const &other) = delete;

        operator bool() const noexcept { return cxt_ptr.first; }

        R call_and_pop(Args... args) noexcept {
            rb::detail::ScopeGaurd const free_function_cxt = {[&] {
                reader_pos->store(cxt_ptr.second, std::memory_order::release);
                cxt_ptr.first = nullptr;
            }};

            return (*cxt_ptr.first) (std::forward<Args>(args)...);
        }

        FunctionHandle &operator=(FunctionHandle &&other) noexcept {
            if (*this) release();
            cxt_ptr = std::exchange(other.cxt_ptr, {nullptr, nullptr});
            reader_pos = other.reader_pos;
            return *this;
        }

        ~FunctionHandle() {
            if (*this) release();
        }

    private:
        FunctionHandle(CxtPtr cxt_ptr, ReaderPos *reader_pos) noexcept : cxt_ptr{cxt_ptr}, reader_pos{reader_pos} {}

        void release() {
            if constexpr (destroyNonInvoked) cxt_ptr.first->destroyFO();
            reader_pos->store(cxt_ptr.second, std::memory_order::release);
        }

        friend class FunctionQueue_MCSP;
        CxtPtr cxt_ptr{nullptr, nullptr};
        ReaderPos *reader_pos;
    };

    class Reader {
    public:
        auto get_function_handle() const noexcept { return FunctionHandle{m_FunctionQueue->get_cxt(), m_ReaderPos}; }

        auto get_function_handle(rb::check_once_tag) const noexcept {
            return FunctionHandle{m_FunctionQueue->get_cxt_check_once(), m_ReaderPos};
        }

        ~Reader() { m_ReaderPos->store(nullptr, std::memory_order::release); }

        Reader(Reader const &) = delete;
        Reader &operator=(Reader const &) = delete;
        Reader(Reader &&) = delete;
        Reader &operator=(Reader &&) = delete;

    private:
        Reader(FunctionQueue_MCSP const *functionQueue, ReaderPos *reader_pos) noexcept
            : m_FunctionQueue{functionQueue}, m_ReaderPos{reader_pos} {}

        friend class FunctionQueue_MCSP;
        FunctionQueue_MCSP const *m_FunctionQueue;
        ReaderPos *m_ReaderPos;
    };

    FunctionQueue_MCSP(uint32_t buffer_size, uint16_t reader_threads, allocator_type allocator = {}) noexcept
        : m_BufferEndOffset{static_cast<uint32_t>(buffer_size - sentinel_region_size)}, m_ReaderThreads{reader_threads},
          m_Buffer{static_cast<std::byte *>(allocator.allocate_bytes(buffer_size, buffer_alignment))},
          m_ReaderPosArray{getReaderPosArray(allocator, reader_threads)}, m_Allocator{allocator} {
        resetReaderPosArray();
    }

    ~FunctionQueue_MCSP() {
        if constexpr (destroyNonInvoked) destroyAllNonInvoked();
        m_Allocator.deallocate_object(m_Buffer, buffer_size());
        m_Allocator.deallocate_object(std::bit_cast<CacheLine *>(m_ReaderPosArray[0]), m_ReaderThreads);
    }

    size_t buffer_size() const noexcept { return m_BufferEndOffset + sentinel_region_size; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire).getValue() ==
               m_OutputReadOffset.load(std::memory_order::relaxed).getValue();
    }

    void clear() noexcept {
        if constexpr (destroyNonInvoked) destroyAllNonInvoked();

        m_InputOffset.store({}, std::memory_order::relaxed);
        m_OutputFollowOffset = 0;
        m_OutputReadOffset.store({}, std::memory_order::relaxed);
        resetReaderPosArray();
    }

    Reader getReader(uint16_t thread_index) const noexcept {
        auto const reader_pos = m_ReaderPosArray[thread_index];
        reader_pos->store(m_Buffer + m_OutputReadOffset.load(std::memory_order::relaxed).getValue(),
                          std::memory_order::release);
        return Reader{this, reader_pos};
    }

    template<typename T>
    bool push(T &&callable) noexcept {
        using Callable = std::decay_t<T>;
        return emplace<Callable>(std::forward<T>(callable));
    }

    template<typename Callable, typename... CArgs>
    requires is_valid_callable_v<Callable, CArgs...>
    bool emplace(CArgs &&...args) noexcept {
        constexpr bool is_callable_empty = std::is_empty_v<Callable>;
        constexpr size_t callable_align = is_callable_empty ? 1 : alignof(Callable);
        constexpr size_t callable_size = is_callable_empty ? 0 : sizeof(Callable);

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const storage = getStorage<callable_align, callable_size>(input_offset.getValue());

        if (!storage) return false;
        storage.construct(rb::detail::type<Callable>, std::forward<CArgs>(args)...);

        uint32_t const next_offset = storage.getNextAddr() - m_Buffer;
        auto const nextInputOffset = input_offset.getIncrTagged(next_offset < m_BufferEndOffset ? next_offset : 0);
        m_InputOffset.store(nextInputOffset, std::memory_order::release);

        if (nextInputOffset.getTag() == 0) {
            auto output_offset = nextInputOffset;
            while (!m_OutputReadOffset.compare_exchange_weak(output_offset,
                                                             nextInputOffset.getSameTagged(output_offset.getValue()),
                                                             std::memory_order::relaxed, std::memory_order::relaxed))
                ;
        }

        return true;
    }

    void clean_memory() noexcept {
        constexpr auto MAX_IDX = std::numeric_limits<uint32_t>::max();
        auto const currentFollowIndex = m_OutputFollowOffset;
        auto const currentReadIndex = m_OutputReadOffset.load(std::memory_order::acquire).getValue();

        auto less_idx = MAX_IDX, gequal_idx = MAX_IDX;
        for (uint16_t t = 0; t != m_ReaderThreads; ++t) {
            if (auto const output_pos = m_ReaderPosArray[t]->load(std::memory_order::acquire)) {
                auto const output_index = static_cast<uint32_t>(output_pos - m_Buffer);
                if (output_index >= currentFollowIndex) gequal_idx = std::min(gequal_idx, output_index);
                else
                    less_idx = std::min(less_idx, output_index);
            }
        }

        m_OutputFollowOffset =
                (gequal_idx != MAX_IDX) ? gequal_idx : ((less_idx != MAX_IDX) ? less_idx : currentReadIndex);
    }


private:
    template<typename Callable>
    static R invokeAndDestroy(void *data, Args... args) noexcept {
        auto &callable = *static_cast<Callable *>(data);
        rb::detail::ScopeGaurd const destroy_functor{[&] { std::destroy_at(&callable); }};

        return std::invoke(callable, static_cast<Args>(args)...);
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
    }

    CxtPtr get_cxt() const noexcept {
        constexpr auto null_cxt = CxtPtr{nullptr, nullptr};
        auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);
        FunctionContext *fcxt_ptr;
        rb::detail::TaggedUint32 next_output_offset;

        do {
            if (input_offset.getTag() < output_offset.getTag() || input_offset.getValue() == output_offset.getValue())
                return null_cxt;
            fcxt_ptr = std::bit_cast<FunctionContext *>(m_Buffer + output_offset.getValue());
            auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
            next_output_offset = input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);
        } while (!m_OutputReadOffset.compare_exchange_weak(output_offset, next_output_offset,
                                                           std::memory_order::relaxed, std::memory_order::relaxed));
        return {fcxt_ptr, m_Buffer + next_output_offset.getValue()};
    }

    CxtPtr get_cxt_check_once() const noexcept {
        constexpr auto null_cxt = CxtPtr{nullptr, nullptr};
        auto output_offset = m_OutputReadOffset.load(std::memory_order::relaxed);
        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

        if (input_offset.getTag() < output_offset.getTag() || input_offset.getValue() == output_offset.getValue())
            return null_cxt;

        auto const fcxt_ptr = std::bit_cast<FunctionContext *>(m_Buffer + output_offset.getValue());
        auto const next_offset = output_offset.getValue() + fcxt_ptr->getStride();
        auto const next_output_offset = input_offset.getSameTagged(next_offset < m_BufferEndOffset ? next_offset : 0);

        if (m_OutputReadOffset.compare_exchange_strong(output_offset, next_output_offset, std::memory_order::relaxed,
                                                       std::memory_order::relaxed))
            return {fcxt_ptr, m_Buffer + next_output_offset.getValue()};
        else
            return null_cxt;
    }

    void destroyAllNonInvoked() {
        auto destroyAndGetStride = [](auto pos) {
            auto const &functionCxt = *std::bit_cast<FunctionContext *>(pos);
            functionCxt.destroyFO();
            return functionCxt.getStride();
        };

        auto output_pos = m_Buffer + m_OutputReadOffset.load(std::memory_order::relaxed).getValue();
        auto const buffer_end = m_Buffer + m_BufferEndOffset;
        auto const input_pos = m_Buffer + m_InputOffset.load(std::memory_order::acquire).getValue();

        if (input_pos == output_pos) return;

        if (output_pos > input_pos) {
            while (output_pos < buffer_end) output_pos += destroyAndGetStride(output_pos);
            output_pos = m_Buffer;
        }

        while (output_pos != input_pos) output_pos += destroyAndGetStride(output_pos);
    }

    template<size_t obj_align, size_t obj_size>
    Storage getStorage(uint32_t input_offset) const noexcept {
        auto const input_pos = m_Buffer + input_offset;
        auto const output_pos = m_Buffer + m_OutputFollowOffset;
        auto const buffer_end = m_Buffer + m_BufferEndOffset;

        constexpr auto getAlignedStorage = Storage::template getAlignedStorage<obj_align, obj_size>;
        if (auto const storage = getAlignedStorage(input_pos);
            (storage.getNextAddr() < output_pos) ||
            ((input_pos >= output_pos) && ((storage.getNextAddr() < buffer_end) || (output_pos != m_Buffer))))
            return storage;

        return {};
    }

    void resetReaderPosArray() noexcept {
        for (uint16_t t = 0; t != m_ReaderThreads; ++t) m_ReaderPosArray[t]->store(nullptr, std::memory_order::relaxed);
    }

    static auto getReaderPosArray(allocator_type allocator, uint16_t threads) noexcept {
        ReaderPosPtrArray readerPosArray;
        auto const cache_line_array = allocator.allocate_object<CacheLine>(threads);
        for (uint16_t t = 0; t != threads; ++t) readerPosArray[t] = std::bit_cast<ReaderPos *>(cache_line_array + t);
        return readerPosArray;
    }

private:
    static constexpr uint32_t INVALID_INDEX{std::numeric_limits<uint32_t>::max()};

    std::atomic<rb::detail::TaggedUint32> m_InputOffset{};
    uint32_t m_OutputFollowOffset{};
    mutable std::atomic<rb::detail::TaggedUint32> m_OutputReadOffset{};

    uint32_t const m_BufferEndOffset;
    uint32_t const m_ReaderThreads;
    std::byte *const m_Buffer;
    ReaderPosPtrArray const m_ReaderPosArray;
    allocator_type m_Allocator;
};

#endif
