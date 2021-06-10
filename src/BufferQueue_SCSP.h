#ifndef FUNCTIONQUEUE_BUFFERQUEUE_SCSP_H
#define FUNCTIONQUEUE_BUFFERQUEUE_SCSP_H

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

template<bool isReadProtected, bool isWriteProtected, size_t buffer_align = sizeof(std::max_align_t)>
class BufferQueue_SCSP {

private:
    class Storage;

    struct DataContext {
    private:
        uint32_t const buffer_size;
        uint32_t const stride;

        friend class Storage;

        DataContext(uint32_t buffer_size, uint32_t stride) noexcept : buffer_size{buffer_size}, stride{stride} {}

        explicit DataContext(uint32_t buffer_size) noexcept : buffer_size{buffer_size},
                                                              stride{static_cast<uint32_t>(
                                                                      align<std::byte, buffer_align>(this + 1) -
                                                                      std::bit_cast<std::byte *>(this) +
                                                                      buffer_size)} {}

    public:
        inline std::pair<std::byte *, uint32_t> getBuffer() noexcept {
            return {align<std::byte, buffer_align>(this + 1), buffer_size};
        }

        inline auto getNextAddr() noexcept { return std::bit_cast<std::byte *>(this) + stride; }
    };

    struct Storage {
        DataContext *const dc_ptr{};
        std::byte *const buffer{};
        uint32_t const avl_size{};

        Storage() noexcept = default;

        Storage(void *dc_ptr, void *buffer, uint32_t avl_size) noexcept : dc_ptr{static_cast<DataContext *>(dc_ptr)},
                                                                          buffer{static_cast<std::byte *>(buffer)},
                                                                          avl_size{avl_size} {}

        inline explicit operator bool() const noexcept {
            return dc_ptr && buffer;
        }

        [[nodiscard]] inline auto createContext(uint32_t buffer_size) const noexcept {
            new (dc_ptr) DataContext{buffer_size, static_cast<uint32_t>(buffer - std::bit_cast<std::byte *>(dc_ptr) +
                                                                        buffer_size)};
            return dc_ptr;
        }

        inline static auto createContext(void *addr, uint32_t buffer_size) noexcept {
            new (addr) DataContext{buffer_size};
            return static_cast<DataContext *>(addr);
        }
    };

    class Null {
    public:
        template<typename... T>
        explicit Null(T &&...) noexcept {}
    };

public:
    class Buffer {
    public:
        [[nodiscard]] inline std::pair<std::byte *, uint32_t> get() const noexcept {
            return getDataContext()->getBuffer();
        }

        Buffer(Buffer &&other) noexcept : buffer_queue{std::exchange(other.buffer_queue, nullptr)} {}

        Buffer(Buffer const &) = delete;

        Buffer &operator=(Buffer &&other) noexcept {
            this->~Buffer();
            buffer_queue = std::exchange(other.buffer_queue, nullptr);
            return *this;
        }

        ~Buffer() noexcept {
            if (buffer_queue) {
                uint32_t const nextOffset = getDataContext()->getNextAddr() - buffer_queue->m_Buffer;
                buffer_queue->m_Remaining.fetch_sub(1, std::memory_order_relaxed);
                if constexpr (isReadProtected) {
                    buffer_queue->m_OutPutOffset.store(nextOffset, std::memory_order_relaxed);
                    buffer_queue->m_ReadFlag.clear(std::memory_order_release);
                } else
                    buffer_queue->m_OutPutOffset.store(nextOffset, std::memory_order_release);
            }
        }

    private:
        friend class BufferQueue_SCSP;

        explicit Buffer(BufferQueue_SCSP const *buffer_queue) noexcept : buffer_queue{buffer_queue} {}

        auto getDataContext() const noexcept {
            return std::bit_cast<DataContext *>(
                    buffer_queue->m_Buffer + buffer_queue->m_OutPutOffset.load(std::memory_order_relaxed));
        }

        BufferQueue_SCSP const *buffer_queue;
    };

public:
    BufferQueue_SCSP(void *memory, std::size_t size) noexcept : m_Buffer{static_cast<std::byte *const>(memory)},
                                                                m_BufferSize{static_cast<uint32_t>(size)} {
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
    }

    inline auto buffer_size() const noexcept { return m_BufferSize; }

    inline auto size() const noexcept { return m_Remaining.load(std::memory_order_relaxed); }

    void clear() noexcept {
        m_InputOffset = 0;
        m_CurrentBufferCxtOffset = 0;
        m_OutPutOffset.store(0, std::memory_order_relaxed);
        m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
        m_Remaining.store(0, std::memory_order_relaxed);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
    }

    inline bool reserve() const noexcept {
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

    template<typename F>
    inline decltype(auto) consume(F &&functor) const noexcept {
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order_relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);

        auto const data_cxt = align<DataContext>(m_Buffer + (found_sentinel ? 0 : output_offset));
        auto const [buffer_data, buffer_size] = data_cxt->getBuffer();

        auto decr_rem_incr_output_offset = [&, nextOffset{data_cxt->getNextAddr() - m_Buffer}] {
            m_Remaining.fetch_sub(1, std::memory_order_relaxed);
            if constexpr (isReadProtected) {
                m_OutPutOffset.store(nextOffset, std::memory_order_relaxed);
                m_ReadFlag.clear(std::memory_order_release);
            } else
                m_OutPutOffset.store(nextOffset, std::memory_order_release);
        };

        using ReturnType = decltype(functor(std::declval<std::byte *>(), uint32_t{}));
        if constexpr (std::is_same_v<void, ReturnType>) {
            std::forward<F>(functor)(buffer_data, buffer_size);
            decr_rem_incr_output_offset();
        } else {
            auto &&result{std::forward<F>(functor)(buffer_data, buffer_size)};
            decr_rem_incr_output_offset();
            return std::forward<decltype(result)>(result);
        }
    }

    inline auto consume() const noexcept {
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order_relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);

        m_OutPutOffset.store(
                align<std::byte, alignof(DataContext)>(m_Buffer + (found_sentinel ? 0 : output_offset)) - m_Buffer,
                std::memory_order_relaxed);

        return Buffer{this};
    }

    inline std::pair<std::byte *, uint32_t> allocate(uint32_t buffer_size) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return {nullptr, 0};
        }

        auto const storage = getMemory(buffer_size);
        if (!storage) {
            if constexpr (isWriteProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return {nullptr, 0};
        }

        m_CurrentBufferCxtOffset = std::bit_cast<std::byte *>(storage.dc_ptr) - m_Buffer;

        return {storage.buffer, storage.avl_size};
    }

    void release(uint32_t bytes_used) noexcept {
        auto const dc_ptr = Storage::createContext(m_Buffer + m_CurrentBufferCxtOffset, bytes_used);
        m_InputOffset = dc_ptr->getNextAddr() - m_Buffer;
        m_Remaining.fetch_add(1, std::memory_order_release);

        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }
    }

    template<typename F>
    inline void allocate_and_release(uint32_t buffer_size, F &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return;
        }

        auto const storage = getMemory(buffer_size);
        if (!storage) {
            if constexpr (isWriteProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return;
        }

        auto const dc_ptr = storage.createContext(
                std::forward<F>(functor)(storage.buffer, storage.avl_size));
        m_InputOffset = dc_ptr->getNextAddr() - m_Buffer;
        m_Remaining.fetch_add(1, std::memory_order_release);

        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }
    }

    [[nodiscard]] inline uint32_t available_space() const noexcept {
        auto getUsableSpace = [](void *buffer, size_t size) noexcept -> uint32_t {
            auto const fp_storage = std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
            size -= sizeof(DataContext);
            auto const callable_storage = std::align(buffer_align, 0, buffer, size);
            return fp_storage && callable_storage ? static_cast<uint32_t>(size) : 0;
        };

        auto const remaining = m_Remaining.load(std::memory_order_acquire);
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);

        if ((input_offset > output_offset) || (input_offset == output_offset && !remaining)) {
            return std::max(getUsableSpace(m_Buffer + input_offset, m_BufferSize - input_offset),
                            getUsableSpace(m_Buffer, output_offset));
        } else if (input_offset < output_offset) {
            return getUsableSpace(m_Buffer + input_offset, output_offset - input_offset);
        } else
            return 0;
    }

private:
    template<typename T, size_t _align = alignof(T)>
    static constexpr inline T *align(void *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + _align) & -_align);
    }

    inline Storage getMemory(uint32_t size) noexcept {
        auto getAlignedStorage = [buffer_size{size}](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
            size -= sizeof(DataContext);
            auto const callable_storage = std::align(buffer_align, buffer_size, buffer, size);
            return {fp_storage, callable_storage, static_cast<uint32_t>(size)};
        };

        auto const remaining = m_Remaining.load(std::memory_order_acquire);
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);

        auto const search_ahead = (input_offset > output_offset) || (input_offset == output_offset && !remaining);

        if (size_t const buffer_size = m_BufferSize - input_offset;
            search_ahead && buffer_size) {
            if (auto storage = getAlignedStorage(m_Buffer + input_offset, buffer_size)) {
                return storage;
            }
        }

        {
            auto const mem = search_ahead ? 0 : input_offset;
            if (size_t const buffer_size = output_offset - mem) {
                if (auto storage = getAlignedStorage(m_Buffer + mem, buffer_size)) {
                    if (search_ahead) {
                        m_SentinelRead.store(input_offset, std::memory_order_relaxed);
                    }
                    return storage;
                }
            }
        }

        return {};
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    uint32_t m_InputOffset{0};
    uint32_t m_CurrentBufferCxtOffset{0};
    mutable std::atomic<uint32_t> m_OutPutOffset{0};
    mutable std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};
    mutable std::atomic<uint32_t> m_Remaining{0};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, Null> m_ReadFlag;

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;
};

#endif//FUNCTIONQUEUE_BUFFERQUEUE_SCSP_H