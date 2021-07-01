#ifndef BUFFERQUEUE_SCSP
#define BUFFERQUEUE_SCSP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

template<bool isReadProtected, bool isWriteProtected, size_t buffer_align = sizeof(std::max_align_t)>
class BufferQueue_SCSP {
private:
    class Null {
    public:
        template<typename... T>
        explicit Null(T &&...) noexcept {}
    };

    class Storage;

    class DataContext {
    public:
        inline std::pair<std::byte *, uint32_t> getBuffer() const noexcept {
            return {align<std::byte, buffer_align>(this + 1), buffer_size};
        }

        inline auto getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + getStride(); }

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
        uint32_t const avl_size{};

        Storage() noexcept = default;

        Storage(void *dc_ptr, void *buffer, uint32_t avl_size) noexcept
            : dc_ptr{static_cast<DataContext *>(dc_ptr)}, buffer{static_cast<std::byte *>(buffer)}, avl_size{avl_size} {
        }

        inline explicit operator bool() const noexcept { return dc_ptr && buffer; }

        [[nodiscard]] inline auto createContext(uint32_t buffer_size) const noexcept {
            return new (dc_ptr) DataContext{buffer_size};
        }

        inline static auto createContext(void *addr, uint32_t buffer_size) noexcept {
            return new (addr) DataContext{buffer_size};
        }
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
                uint32_t const next_output_offset = getDataContext()->getNextAddr() - buffer_queue->m_Buffer;
                buffer_queue->m_OutputOffset.store(next_output_offset, std::memory_order::release);
                if constexpr (isReadProtected) buffer_queue->m_ReadFlag.clear(std::memory_order::release);
            }
        }

    private:
        friend class BufferQueue_SCSP;

        explicit Buffer(BufferQueue_SCSP const *buffer_queue) noexcept : buffer_queue{buffer_queue} {}

        auto getDataContext() const noexcept {
            return std::bit_cast<DataContext *>(buffer_queue->m_Buffer +
                                                buffer_queue->m_OutPutOffset.load(std::memory_order::relaxed));
        }

        BufferQueue_SCSP const *buffer_queue;
    };

    BufferQueue_SCSP(void *memory, std::size_t size) noexcept
        : m_Buffer{static_cast<std::byte *const>(memory)}, m_BufferSize{static_cast<uint32_t>(size)} {
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
    }

    ~BufferQueue_SCSP() noexcept = default;

    inline auto buffer_size() const noexcept { return m_BufferSize; }

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

    inline bool reserve() noexcept {
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

    template<typename F>
    inline decltype(auto) consume(F &&functor) const noexcept {
        auto const output_offset = m_OutputOffset.load(std::memory_order::acquire);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order::relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);

        auto const data_cxt = align<DataContext>(m_Buffer + (found_sentinel ? 0 : output_offset));
        auto const [buffer_data, buffer_size] = data_cxt->getBuffer();

        auto incr_output_offset = [this, next_output_offset{data_cxt->getNextAddr() - m_Buffer}] {
            m_OutputOffset.store(next_output_offset, std::memory_order::release);
            if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order::release);
        };

        using ReturnType = decltype(functor(std::declval<std::byte *>(), uint32_t{}));
        if constexpr (std::is_void_v<ReturnType>) {
            std::forward<F>(functor)(buffer_data, buffer_size);
            incr_output_offset();
        } else {
            auto &&result{std::forward<F>(functor)(buffer_data, buffer_size)};
            incr_output_offset();
            return std::forward<decltype(result)>(result);
        }
    }

    inline auto consume() const noexcept {
        auto const output_offset = m_OutputOffset.load(std::memory_order::relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order::relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);

        auto const data_cxt_offset =
                align<std::byte, alignof(DataContext)>(m_Buffer + (found_sentinel ? 0 : output_offset)) - m_Buffer;
        m_OutputOffset.store(data_cxt_offset, std::memory_order::relaxed);

        return Buffer{this};
    }

    inline std::pair<std::byte *, uint32_t> allocate(uint32_t buffer_size) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return {nullptr, 0};
        }

        auto const storage = getStorage(buffer_size);
        if (!storage) {
            if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order::release); }
            return {nullptr, 0};
        }

        m_CurrentBufferCxtOffset = std::bit_cast<std::byte *>(storage.dc_ptr) - m_Buffer;

        return {storage.buffer, storage.avl_size};
    }

    void release(uint32_t bytes_used) noexcept {
        auto const dc_ptr = Storage::createContext(m_Buffer + m_CurrentBufferCxtOffset, bytes_used);

        auto const next_input_offset = dc_ptr->getNextAddr() - m_Buffer;
        m_InputOffset.store(next_input_offset, std::memory_order::release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order::release); }
    }

    template<typename F>
    inline uint32_t allocate_and_release(uint32_t buffer_size, F &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return 0;
        }

        auto const storage = getStorage(buffer_size);
        if (!storage) {
            if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order::release); }
            return 0;
        }

        auto const bytes_used = std::forward<F>(functor)(storage.buffer, storage.avl_size);
        auto const dc_ptr = storage.createContext(bytes_used);

        auto const next_input_offset = dc_ptr->getNextAddr() - m_Buffer;
        m_InputOffset.store(next_input_offset, std::memory_order::release);

        if constexpr (isWriteProtected) { m_WriteFlag.clear(std::memory_order::release); }

        return bytes_used;
    }

    [[nodiscard]] inline uint32_t available_space() const noexcept {
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
                            getUsableSpace(m_Buffer, output_offset - bool{output_offset}));
        } else if (input_offset < output_offset) {
            return getUsableSpace(m_Buffer + input_offset, output_offset - input_offset - 1);
        } else
            return 0;
    }

private:
    template<typename T, size_t _align = alignof(T)>
    static constexpr inline T *align(void const *ptr) noexcept {
        return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + _align) & -_align);
    }

    inline Storage __attribute__((always_inline)) getStorage(size_t size) noexcept {

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

            if (output_offset)
                if (auto const storage = getAlignedStorage(m_Buffer, output_offset - 1)) {
                    m_SentinelRead.store(input_offset, std::memory_order::relaxed);
                    return storage;
                }

        } else {
            return getAlignedStorage(m_Buffer + input_offset, output_offset - input_offset - 1);
        }

        return {};
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    std::atomic<uint32_t> m_InputOffset{0};
    uint32_t m_CurrentBufferCxtOffset{0};

    mutable std::atomic<uint32_t> m_OutputOffset{0};
    mutable std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;
    [[no_unique_address]] mutable std::conditional_t<isReadProtected, std::atomic_flag, Null> m_ReadFlag;

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;
};

#endif
