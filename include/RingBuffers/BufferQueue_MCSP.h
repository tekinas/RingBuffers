#ifndef BUFFERQUEUE_MCSP
#define BUFFERQUEUE_MCSP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

template<bool isWriteProtected, size_t buffer_align = sizeof(std::max_align_t)>
class BufferQueue_MCSP {
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

    struct DataContext {
    public:
        std::span<std::byte> getBuffer() const noexcept {
            return {align<std::byte, buffer_align>(this + 1), buffer_size.load(std::memory_order::relaxed)};
        }

        std::byte *getNextAddr() const noexcept { return std::bit_cast<std::byte *>(this) + getStride(); }

        std::byte *getNextAddrFree() const noexcept { return std::bit_cast<std::byte *>(this) + getStrideFree(); }

        [[nodiscard]] bool isFree() const noexcept { return buffer_size.load(std::memory_order::acquire) == 0; }

        void free() noexcept {
            *std::bit_cast<uint32_t *>(&buffer_size + 1) = getStride();
            buffer_size.store(0, std::memory_order::release);
        }

    private:
        explicit DataContext(uint32_t buffer_size) noexcept : buffer_size{buffer_size} {}

        uint32_t getStride() const noexcept {
            return static_cast<uint32_t>(align<std::byte, buffer_align>(this + 1) - std::bit_cast<std::byte *>(this) +
                                         buffer_size.load(std::memory_order::relaxed));
        }

        uint32_t getStrideFree() const noexcept { return *std::bit_cast<uint32_t *>(&buffer_size + 1); }

        friend class Storage;

        std::atomic<uint32_t> buffer_size;
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
        [[nodiscard]] std::span<std::byte> get() const noexcept { return dataContext->getBuffer(); }

        operator bool() const noexcept { return dataContext; }

        Buffer() noexcept = default;

        Buffer(Buffer &&other) noexcept : dataContext{std::exchange(other.dataContext, nullptr)} {}

        Buffer &operator=(Buffer &&other) noexcept {
            std::destroy_at(this);
            dataContext = std::exchange(other.dataContext, nullptr);
            return *this;
        }

        ~Buffer() noexcept {
            if (dataContext) dataContext->free();
        }

    private:
        friend class BufferQueue_MCSP;

        explicit Buffer(DataContext *dataContext) noexcept : dataContext{dataContext} {}

        DataContext *dataContext{};
    };

    BufferQueue_MCSP(std::byte *memory, size_t size) noexcept
        : m_BufferSize{static_cast<uint32_t>(size)}, m_Buffer{memory} {}

    ~BufferQueue_MCSP() noexcept = default;

    uint32_t buffer_size() const noexcept { return m_BufferSize; }

    bool empty() const noexcept {
        return m_InputOffset.load(std::memory_order::acquire) == m_OutputHeadOffset.load(std::memory_order::acquire);
    }

    void clear() noexcept {
        m_InputOffset.store({}, std::memory_order::relaxed);
        m_CurrentBufferCxtOffset = 0;
        m_OutputTailOffset = 0;
        m_SentinelFollow = NO_SENTINEL;

        m_OutputHeadOffset.store({}, std::memory_order::relaxed);
        m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::relaxed);
    }

    Buffer consume() const noexcept { return Buffer{get_data_cxt()}; }

    template<typename F>
    uint32_t consume(F &&functor) const noexcept {
        if (auto const data_cxt = get_data_cxt()) {
            auto const buffer = data_cxt->getBuffer();
            std::forward<F>(functor)(buffer);
            data_cxt->free();
            return buffer.size();
        } else
            return 0;
    }

    template<typename Functor>
    uint32_t consume_all(Functor &&functor) const noexcept {
        uint32_t bytes_consumed{0};
        while (true) {
            if (auto const data_cxt = get_data_cxt()) {
                auto const buffer = data_cxt->getBuffer();
                std::forward<Functor>(functor)(buffer);
                data_cxt->free();
                bytes_consumed += buffer.size();
            } else
                return bytes_consumed;
        }
    }

    std::span<std::byte> allocate(uint32_t bytes_req) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return {};
        }

        auto const storage = getStorage(m_InputOffset.load(std::memory_order::relaxed).getValue(), bytes_req);
        if (!storage) {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
            return {};
        }

        m_CurrentBufferCxtOffset = std::bit_cast<std::byte *>(storage.dc_ptr) - m_Buffer;

        return {storage.buffer, storage.bytes_avl};
    }

    void release(uint32_t bytes_allocated) noexcept {
        auto const nextInputOffsetValue =
                Storage::createContext(m_Buffer + m_CurrentBufferCxtOffset, bytes_allocated) - m_Buffer;
        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const nextInputOffset = input_offset.getIncrTagged(nextInputOffsetValue);

        m_InputOffset.store(nextInputOffset, std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);

        if (nextInputOffset.getTag() == 0) {
            auto output_offset = nextInputOffset;
            while (!m_OutputHeadOffset.compare_exchange_weak(output_offset,
                                                             nextInputOffset.getSameTagged(output_offset.getValue()),
                                                             std::memory_order::relaxed, std::memory_order::relaxed))
                ;
        }
    }

    template<typename F>
    uint32_t allocate_and_release(uint32_t bytes_req, F &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order::acquire)) return 0;
        }

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed);
        auto const storage = getStorage(input_offset.getValue(), bytes_req);
        if (!storage) {
            if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);
            return 0;
        }

        auto const available_storage = std::span<std::byte>{storage.buffer, storage.bytes_avl};
        auto const bytes_allocated = std::forward<F>(functor)(available_storage);
        auto const nextInputOffsetValue = storage.createContext(bytes_allocated) - m_Buffer;

        auto const nextInputOffset = input_offset.getIncrTagged(nextInputOffsetValue);

        m_InputOffset.store(nextInputOffset, std::memory_order::release);

        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order::release);

        if (nextInputOffset.getTag() == 0) {
            auto output_offset = nextInputOffset;
            while (!m_OutputHeadOffset.compare_exchange_weak(output_offset,
                                                             nextInputOffset.getSameTagged(output_offset.getValue()),
                                                             std::memory_order::relaxed, std::memory_order::relaxed))
                ;
        }

        return bytes_allocated;
    }

    [[nodiscard]] FORCE_INLINE uint32_t available_space() noexcept {
        auto getUsableSpace = [](void *buffer, size_t size) noexcept -> uint32_t {
            auto const fp_storage = std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
            size -= sizeof(DataContext);
            auto const callable_storage = std::align(buffer_align, 0, buffer, size);
            return fp_storage && callable_storage ? static_cast<uint32_t>(size) : 0;
        };

        auto const input_offset = m_InputOffset.load(std::memory_order::relaxed).getValue();
        auto const output_offset = cleanMemory();

        if (input_offset >= output_offset) {
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

    FORCE_INLINE DataContext *get_data_cxt() const noexcept {
        DataContext *dataCxtPtr;
        TaggedUint32 nextOutputOffset;
        bool found_sentinel;

        auto output_offset = m_OutputHeadOffset.load(std::memory_order::relaxed);

        do {
            auto const input_offset = m_InputOffset.load(std::memory_order::acquire);

            if (input_offset.getTag() < output_offset.getTag()) return nullptr;
            if (input_offset.getValue() == output_offset.getValue()) return nullptr;

            found_sentinel = output_offset.getValue() == m_SentinelRead.load(std::memory_order::relaxed);
            dataCxtPtr = align<DataContext>(m_Buffer + (found_sentinel ? 0 : output_offset.getValue()));

            uint32_t const nextOffset = dataCxtPtr->getNextAddr() - m_Buffer;
            nextOutputOffset = input_offset.getSameTagged(nextOffset);

        } while (!m_OutputHeadOffset.compare_exchange_weak(output_offset, nextOutputOffset, std::memory_order::relaxed,
                                                           std::memory_order::relaxed));

        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order::relaxed);
        return dataCxtPtr;
    }

    FORCE_INLINE uint32_t cleanMemory() noexcept {
        auto const output_head = m_OutputHeadOffset.load(std::memory_order::relaxed).getValue();
        auto output_tail = m_OutputTailOffset;

        if (output_tail != output_head) {
            while (output_tail != output_head) {
                if (output_tail == m_SentinelFollow) {
                    if (m_SentinelRead.load(std::memory_order::relaxed) == NO_SENTINEL) {
                        output_tail = 0;
                        m_SentinelFollow = NO_SENTINEL;
                    } else
                        break;
                } else if (auto const &dataCxt = *align<DataContext>(m_Buffer + output_tail); dataCxt.isFree()) {
                    output_tail = dataCxt.getNextAddrFree() - m_Buffer;
                } else {
                    break;
                }
            }

            m_OutputTailOffset = output_tail;
        }

        return output_tail;
    }

    FORCE_INLINE Storage getStorage(uint32_t const input_offset, size_t const size) noexcept {
        auto getAlignedStorage = [buffer_size{size}](void *buffer, size_t size) noexcept -> Storage {
            auto const dc_storage = std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
            size -= sizeof(DataContext);
            auto const b_storage = std::align(buffer_align, buffer_size, buffer, size);
            return {dc_storage, b_storage, static_cast<uint32_t>(size)};
        };

        auto const output_offset = cleanMemory();

        if (input_offset >= output_offset) {
            if (auto const storage = getAlignedStorage(m_Buffer + input_offset, m_BufferSize - input_offset - 1)) {
                return storage;
            }

            if (output_offset) {
                if (auto const storage = getAlignedStorage(m_Buffer, output_offset - 1)) {
                    m_SentinelFollow = input_offset;
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

    std::atomic<TaggedUint32> m_InputOffset{};
    uint32_t m_CurrentBufferCxtOffset{0};
    uint32_t m_OutputTailOffset{0};
    uint32_t m_SentinelFollow{NO_SENTINEL};

    mutable std::atomic<TaggedUint32> m_OutputHeadOffset{};
    mutable std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Empty> m_WriteFlag{};

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;
};

#endif
