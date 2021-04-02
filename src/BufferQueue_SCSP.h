#ifndef FUNCTIONQUEUE_BUFFERQUEUE_SCSP_H
#define FUNCTIONQUEUE_BUFFERQUEUE_SCSP_H

#include <atomic>
#include <cstring>
#include <cstddef>
#include <limits>
#include <memory>
#include <functional>


template<bool isReadProtected, bool isWriteProtected, size_t buffer_align = 8>
class BufferQueue_SCSP {

private:
    class Storage;

    struct DataContext {
    private:
        uint32_t buffer_size;
        uint32_t stride;

        friend class Storage;

        DataContext(uint32_t buffer_size, uint32_t stride) noexcept:
                buffer_size{buffer_size}, stride{stride} {}

    public:
        inline auto getReadData() noexcept {
            return std::tuple{getBuffer(), buffer_size, getNextAddr()};
        }

        inline auto getBuffer() noexcept { return align<std::byte, buffer_align>(this + 1); }

        void setBufferSize(uint32_t size) noexcept {
            buffer_size = size;
            stride = align<std::byte, buffer_align>(this + 1) - reinterpret_cast<std::byte *>(this) + buffer_size;
        }

        inline auto getNextAddr() noexcept { return reinterpret_cast<std::byte *>(this) + stride; }
    };

    struct Storage {
        DataContext *const dc_ptr{};
        std::byte *const buffer{};

        Storage() noexcept = default;

        Storage(void *dc_ptr, void *buffer) noexcept: dc_ptr{static_cast<DataContext *>(dc_ptr)},
                                                      buffer{static_cast<std::byte *>(buffer)} {}

        inline explicit operator bool() const noexcept {
            return dc_ptr && buffer;
        }

        [[nodiscard]] inline auto set(uint32_t buffer_size) const noexcept {
            new(dc_ptr) DataContext{buffer_size, static_cast<uint32_t>(buffer - reinterpret_cast<std::byte *>(dc_ptr) +
                                                                       buffer_size)};
            return dc_ptr;
        }
    };

    class Null {
    public:
        template<typename ...T>
        explicit Null(T &&...) noexcept {}
    };

public:
    BufferQueue_SCSP(void *memory, std::size_t size) noexcept: m_Memory{static_cast<std::byte *const>(memory)},
                                                               m_MemorySize{static_cast<uint32_t>(size)} {
        if constexpr (isReadProtected) m_ReadFlag.clear(std::memory_order_relaxed);
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        memset(m_Memory, 0, m_MemorySize);
    }

    inline bool reserve_buffer() noexcept {
        if constexpr (isReadProtected) {
            if (m_ReadFlag.test_and_set(std::memory_order_relaxed)) return false;
            if (!m_Remaining.load(std::memory_order_acquire)) {
                m_ReadFlag.clear(std::memory_order_relaxed);
                return false;
            } else return true;
        } else return m_Remaining.load(std::memory_order_acquire);
    }

    template<typename F>
    inline decltype(auto) consume_buffer(F &&functor) noexcept {
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);
        bool const found_sentinel = output_offset == m_SentinelRead.load(std::memory_order_relaxed);
        if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);

        auto const data_cxt = align<DataContext>(m_Memory + (found_sentinel ? 0 : output_offset));
        auto const[buffer_data, buffer_size, nextAddr] = data_cxt->getReadData();

        auto decr_rem_incr_output_offset = [&, nextOffset{nextAddr - m_Memory}] {
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

    auto get_unreleased_buffer() const noexcept {
        auto const[buffer, size, nextAddr] = reinterpret_cast<DataContext *>(m_Memory +
                                                                             m_CurrentBufferCxtOffset)->getReadData();
        return std::pair{buffer, size};
    }

    void release_buffer(uint32_t bytes_used) noexcept {
        auto const dc_ptr = reinterpret_cast<DataContext *>(m_Memory + m_CurrentBufferCxtOffset);
        dc_ptr->setBufferSize(bytes_used);
        m_InputOffset = dc_ptr->getNextAddr() - m_Memory;
        m_Remaining.fetch_add(1, std::memory_order_release);
    }

    inline std::byte *allocate_buffer(uint32_t buffer_size) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return nullptr;
        }

        Storage const storage = getMemory(buffer_size);
        if (!storage) {
            if constexpr (isWriteProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return nullptr;
        }

        auto const dc_ptr = storage.set(buffer_size);
        m_CurrentBufferCxtOffset = reinterpret_cast<std::byte *>(dc_ptr) - m_Memory;

        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return dc_ptr->getBuffer();
    }

    template<typename F>
    inline void allocate_and_release_buffer(uint32_t buffer_size, F &&functor) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return;
        }

        Storage const storage = getMemory(buffer_size);
        if (!storage) {
            if constexpr (isWriteProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return;
        }

        auto const dc_ptr = storage.set(buffer_size);
        dc_ptr->setBufferSize(std::invoke(std::forward<F>(functor), dc_ptr->getBuffer()));

        m_InputOffset = dc_ptr->getNextAddr() - m_Memory;
        m_Remaining.fetch_add(1, std::memory_order_release);

        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }
    }

    [[nodiscard]] inline uint32_t size() const noexcept { return m_Remaining.load(std::memory_order_relaxed); }

private:

    template<typename T, size_t _align = alignof(T)>
    static constexpr inline T *align(void *ptr) noexcept {
        return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(ptr) - 1u + _align) & -_align);
    }

    inline Storage getMemory(uint32_t size) noexcept {
        auto getAlignedStorage = [buffer_size{size}](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
            size -= sizeof(DataContext);
            auto const callable_storage = std::align(buffer_align, buffer_size, buffer, size);
            return {fp_storage, callable_storage};
        };

        auto const remaining = m_Remaining.load(std::memory_order_acquire);
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);

        auto const search_ahead = (input_offset > output_offset) || (input_offset == output_offset && !remaining);

        if (size_t const buffer_size = m_MemorySize - input_offset;
                search_ahead && buffer_size) {
            if (auto storage = getAlignedStorage(m_Memory + input_offset, buffer_size)) {
                return storage;
            }
        }

        {
            auto const mem = search_ahead ? 0 : input_offset;
            if (size_t const buffer_size = output_offset - mem) {
                if (auto storage = getAlignedStorage(m_Memory + mem, buffer_size)) {
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
    std::atomic<uint32_t> m_OutPutOffset{0};
    std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};
    std::atomic<uint32_t> m_Remaining{0};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;
    [[no_unique_address]] std::conditional_t<isReadProtected, std::atomic_flag, Null> m_ReadFlag;

    uint32_t const m_MemorySize;
    std::byte *const m_Memory;
};

#endif //FUNCTIONQUEUE_BUFFERQUEUE_SCSP_H
