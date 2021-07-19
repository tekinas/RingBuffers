#ifndef BUFFERQUEUE_SCSP
#define BUFFERQUEUE_SCSP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

template<bool isReadProtected, bool isWriteProtected, size_t buffer_align = sizeof(std::max_align_t)>
class BufferQueue_SCSP {
private:
    class Storage;

    class DataContext {
    public:
        std::span<std::byte> getBuffer() const noexcept {
            return {align<std::byte, buffer_align>(this + 1), buffer_size};
        }

        std::byte *getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + getStride(); }

    private:
        explicit DataContext(uint32_t buffer_size) noexcept : buffer_size{buffer_size} {}

        uint32_t getStride() const noexcept {
            return static_cast<uint32_t>(align<std::byte, buffer_align>(this + 1) - std::bit_cast<std::byte *>(this) +
                                         buffer_size);
        }

        friend class Storage;

        uint32_t const buffer_size;
    };

    class Storage {
    public:
        DataContext *const dc_ptr{};
        std::byte *const buffer{};
        uint32_t const bytes_avl{};

        Storage() noexcept = default;

        Storage(void *dc_ptr, void *buffer, uint32_t bytes_avl) noexcept
            : dc_ptr{static_cast<DataContext *>(dc_ptr)}, buffer{static_cast<std::byte *>(buffer)},
              bytes_avl{bytes_avl} {}

        explicit operator bool() const noexcept { return dc_ptr && buffer; }

        [[nodiscard]] std::byte *createContext(uint32_t buffer_size) const noexcept {
            return (new (dc_ptr) DataContext{buffer_size})->getNextAddr();
        }

        static std::byte *createContext(void *addr, uint32_t buffer_size) noexcept {
            return (new (addr) DataContext{buffer_size})->getNextAddr();
        }
    };

public:
    class Buffer {
    public:
        [[nodiscard]] std::span<std::byte> get() const noexcept { return getDataContext()->getBuffer(); }

        operator bool() const noexcept { return buffer_queue; }

        Buffer(Buffer &&other) noexcept : buffer_queue{std::exchange(other.buffer_queue, nullptr)} {}

        Buffer(Buffer const &) = delete;

        Buffer &operator=(Buffer &&other) noexcept {
            this->~Buffer();
            buffer_queue = std::exchange(other.buffer_queue, nullptr);
            return *this;
        }

        ~Buffer() noexcept {
            if (buffer_queue) {
                uint32_t const next_output_offset = getDataContext()->getNextAddr() - buffer_queue->m_Buffer;
                buffer_queue->m_OutputOffset.store(next_output_offset, std::memory_order::release);
                if constexpr (isReadProtected) buffer_queue->m_ReadFlag.clear(std::memory_order::release);
            }
        }

    private:
        friend class BufferQueue_SCSP;

        explicit Buffer(BufferQueue_SCSP const *buffer_queue) noexcept : buffer_queue{buffer_queue} {}

        DataContext *getDataContext() const noexcept {
            return std::bit_cast<DataContext *>(buffer_queue->m_Buffer +
                                                buffer_queue->m_OutPutOffset.load(std::memory_order::relaxed));
        }

        BufferQueue_SCSP const *buffer_queue;
    };

    BufferQueue_SCSP(std::byte *memory, std::size_t size) noexcept
        : m_BufferSize{static_cast<uint32_t>(size)}, m_Buffer{memory} {}

    ~BufferQueue_SCSP() noexcept = default;

    uint32_t buffer_size() const noexcept { return m_BufferSize; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire) == m_OutputOffset.load(std::memory_order::acquire);
    }

    void clear() noexcept {
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

    Buffer consume() const noexcept {
        auto const output_offset = m_OutputOffset.load(std::memory_order::relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order::relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);

        auto const data_cxt_offset =
                align<std::byte, alignof(DataContext)>(m_Buffer + (found_sentinel ? 0 : output_offset)) - m_Buffer;
        m_OutputOffset.store(data_cxt_offset, std::memory_order::relaxed);

        return Buffer{this};
    }

    template<typename F>
    decltype(auto) consume(F &&functor) const noexcept {
        auto const output_offset = m_OutputOffset.load(std::memory_order::acquire);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order::relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);

        auto const data_cxt = align<DataContext>(m_Buffer + (found_sentinel ? 0 : output_offset));
        auto const buffer = data_cxt->getBuffer();
        uint32_t const next_output_offset = data_cxt->getNextAddr() - m_Buffer;

        auto forward_output_offset = [&, next_output_offset] {
            m_OutputOffset.store(next_output_offset, std::memory_order::release);
            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);
        };

        using ReturnType = decltype(std::forward<F>(functor)(std::span<std::byte>{}));
        if constexpr (std::is_void_v<ReturnType>) {
            std::forward<F>(functor)(buffer);
            forward_output_offset();
        } else {
            auto &&result{std::forward<F>(functor)(std::forward<F>(functor)(buffer))};
            forward_output_offset();
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename F>
    uint32_t consume_all(F &&functor) const noexcept {
        auto const output_offset = m_OutputOffset.load(std::memory_order::relaxed);
        auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

        uint32_t bytes_consumed{0};
        auto consume_and_forward = [this, &bytes_consumed, &functor](uint32_t output_offset) {
            auto const data_cxt = align<DataContext>(m_Buffer + output_offset);
            auto const buffer = data_cxt->getBuffer();
            std::forward<F>(functor)(buffer);
            bytes_consumed += buffer.size();
            return data_cxt->getNextAddr() - m_Buffer;
        };

        auto c_output_offset = output_offset;
        if (output_offset > input_offset) {
            auto const sentinel = m_SentinelRead.exchange(NO_SENTINEL, std::memory_order::relaxed);
            while (c_output_offset != sentinel) { c_output_offset = consume_and_forward(c_output_offset); }
            c_output_offset = 0;
        }

        while (c_output_offset != input_offset) { c_output_offset = consume_and_forward(c_output_offset); }

        if (c_output_offset != output_offset) m_OutputOffset.store(c_output_offset, std::memory_order::release);
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);

        return bytes_consumed;
    }

    std::span<std::byte> allocate(uint32_t bytes_req) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return {};
        }

        auto const storage = getStorage(bytes_req);
        if (!storage) {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
            return {};
        }

        m_CurrentBufferCxtOffset = std::bit_cast<std::byte *>(storage.dc_ptr) - m_Buffer;

        return {storage.buffer, storage.avl_size};
    }

    void release(uint32_t bytes_allocated) noexcept {
        uint32_t const next_input_offset =
                Storage::createContext(m_Buffer + m_CurrentBufferCxtOffset, bytes_allocated) - m_Buffer;
        m_InputOffset.store(next_input_offset, std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
    }

    template<typename F>
    uint32_t allocate_and_release(uint32_t bytes_req, F &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return 0;
        }

        auto const storage = getStorage(bytes_req);
        if (!storage) {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
            return 0;
        }

        auto const available_storage = std::span<std::byte>{storage.buffer, storage.bytes_avl};
        auto const bytes_allocated = std::forward<F>(functor)(available_storage);
        uint32_t const next_input_offset = storage.createContext(bytes_allocated) - m_Buffer;

        m_InputOffset.store(next_input_offset, std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);

        return bytes_allocated;
    }

    [[nodiscard]] uint32_t available_space() const noexcept {
        auto getUsableSpace = [](void *buffer, size_t size) noexcept -> uint32_t {
            auto const fp_storage = std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
            size -= sizeof(DataContext);
            auto const callable_storage = std::align(buffer_align, 0, buffer, size);
            return fp_storage && callable_storage ? static_cast<uint32_t>(size) : 0;
        };

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const output_offset = m_OutputOffset.load(std::memory_order::acquire);

        if ((input_offset >= output_offset)) {
            return std::max(getUsableSpace(m_Buffer + input_offset, m_BufferSize - input_offset - 1),
                            getUsableSpace(m_Buffer, output_offset - static_cast<bool>(output_offset)));
        } else if (input_offset < output_offset) {
            return getUsableSpace(m_Buffer + input_offset, output_offset - input_offset - 1);
        } else
            return 0;
    }

private:
    template<typename T, size_t _align = alignof(T)>
    static constexpr T *align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + _align) & -_align);
    }

    FORCE_INLINE Storage getStorage(size_t size) noexcept {
        auto getAlignedStorage = [buffer_size{size}](void *buffer, size_t size) noexcept -> Storage {
            auto const dc_storage = std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
            size -= sizeof(DataContext);
            auto const bc_storage = std::align(buffer_align, buffer_size, buffer, size);
            return {dc_storage, bc_storage, static_cast<uint32_t>(size)};
        };

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const output_offset = m_OutputOffset.load(std::memory_order::acquire);

        if (input_offset >= output_offset) {
            if (auto const storage = getAlignedStorage(m_Buffer + input_offset, m_BufferSize - input_offset - 1)) {
                return storage;
            }

            if (output_offset) {
                if (auto const storage = getAlignedStorage(m_Buffer, output_offset - 1)) {
                    m_SentinelRead.store(input_offset, std::memory_order::relaxed);
                    return storage;
                }
            }

            return {};
        } else {
            return getAlignedStorage(m_Buffer + input_offset, output_offset - input_offset - 1);
        }
    }

private:
    class Empty {
    public:
        template<typename... T>
        explicit Empty(T &&...) noexcept {}
    };

    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    std::atomic<uint32_t> m_InputOffset{0};
    uint32_t m_CurrentBufferCxtOffset{0};

    mutable std::atomic<uint32_t> m_OutputOffset{0};
    mutable std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Empty> m_WriteFlag{};
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, Empty> m_ReadFlag{};

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;
};

#endif
