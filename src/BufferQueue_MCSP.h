//
// Created by tekinas on 4/2/21.
//

#ifndef FUNCTIONQUEUE_BUFFERQUEUE_MCSP_H
#define FUNCTIONQUEUE_BUFFERQUEUE_MCSP_H

#include <atomic>
#include <cstring>
#include <cstddef>
#include <limits>
#include <memory>
#include <functional>


template<bool isWriteProtected, bool isIndexed, size_t buffer_align = 8>
class BufferQueue_MCSP {

private:

    struct IndexedOffset {
    private:
        uint32_t offset{};
        uint32_t index{};

        IndexedOffset(uint32_t offset, uint32_t index) noexcept: offset{offset}, index{index} {}

    public:
        IndexedOffset() noexcept = default;

        explicit operator uint32_t() const noexcept { return offset; }

        inline IndexedOffset getNext(uint32_t _offset) const noexcept {
            return {_offset, index + 1};
        }
    };

    using OffsetType = std::conditional_t<isIndexed, IndexedOffset, uint32_t>;

    class Storage;

    struct DataContext {
    private:
        std::atomic<uint32_t> buffer_size;
        uint32_t stride;

        friend class Storage;

        DataContext(uint32_t buffer_size, uint32_t stride) noexcept:
                buffer_size{buffer_size}, stride{stride} {}

    public:
        inline auto getReadData() noexcept {
            return std::tuple{getBuffer(), buffer_size.load(std::memory_order_relaxed), getNextAddr()};
        }

        inline auto getBuffer() noexcept { return align<std::byte, buffer_align>(this + 1); }

        void setBufferSize(uint32_t size) noexcept {
            buffer_size.store(size, std::memory_order_relaxed);
            stride = align<std::byte, buffer_align>(this + 1) - reinterpret_cast<std::byte *>(this) + size;
        }

        inline auto getNextAddr() noexcept { return reinterpret_cast<std::byte *>(this) + stride; }

        inline void release() noexcept {
            buffer_size.store(0, std::memory_order_release);
        }

        [[nodiscard]] inline bool isReleased() const noexcept {
            return buffer_size.load(std::memory_order_acquire) == 0;
        }
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
    BufferQueue_MCSP(void *memory, std::size_t size) noexcept: m_Memory{static_cast<std::byte *const>(memory)},
                                                               m_MemorySize{static_cast<uint32_t>(size)} {
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        memset(m_Memory, 0, m_MemorySize);
    }

    inline bool reserve_buffer() noexcept {
        uint32_t rem = m_Remaining.load(std::memory_order_relaxed);
        if (!rem) return false;

        while (rem &&
               !m_Remaining.compare_exchange_weak(rem, rem - 1, std::memory_order_acquire, std::memory_order_relaxed));
        return rem;
    }

    template<typename F>
    inline decltype(auto) consume_buffer(F &&functor) noexcept {
        DataContext *data_cxt;
        auto out_pos = m_OutPutOffset.load(std::memory_order::relaxed);
        bool found_sentinel;
        while (!m_OutPutOffset.compare_exchange_weak(out_pos, [&, out_pos] {
            if ((found_sentinel = (uint32_t{out_pos} == m_SentinelRead.load(std::memory_order_relaxed)))) {
                data_cxt = align<DataContext>(m_Memory);
            } else data_cxt = align<DataContext>(m_Memory + uint32_t{out_pos});

            auto const nextOffset = data_cxt->getNextAddr() - m_Memory;
            if constexpr (isIndexed) { return out_pos.getNext(nextOffset); }
            else return nextOffset;
        }(), std::memory_order_relaxed, std::memory_order_relaxed));

        auto const[buffer_data, buffer_size, nextAddr] = data_cxt->getReadData();

        using ReturnType = decltype(functor(std::declval<std::byte *>(), uint32_t{}));
        if constexpr (std::is_same_v<void, ReturnType>) {
            std::forward<F>(functor)(buffer_data, buffer_size);
            data_cxt->release();
            if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
        } else {
            auto &&result{std::forward<F>(functor)(buffer_data, buffer_size)};
            data_cxt->release();
            if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
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
        ++m_RemainingClean;
    }

    inline /*__attribute__((always_inline))*/ std::byte *allocate_buffer(uint32_t buffer_size) noexcept {
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

        ++m_RemainingClean;
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

        auto const remaining = m_RemainingClean;
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutputFollowOffset;

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
                        m_SentinelFollow = input_offset;
                    }
                    return storage;
                }
            }
        }

        cleanMemory();

        return {};
    }

    inline void cleanMemory() noexcept {
        if (m_RemainingClean) {
            auto followOffset = m_OutputFollowOffset;
            auto remaining = m_RemainingClean;
            while (remaining) {
                if (followOffset == m_SentinelFollow) {
                    if (m_SentinelRead.load(std::memory_order_relaxed) == NO_SENTINEL) {
                        followOffset = 0;
                        m_SentinelFollow = NO_SENTINEL;
                    }
                } else if (auto const functionCxt = align<DataContext>(m_Memory + followOffset);
                        functionCxt->isReleased()) {
                    followOffset = functionCxt->getNextAddr() - m_Memory;
                    --remaining;
                } else {
                    break;
                }
            }
            m_RemainingClean = remaining;
            m_OutputFollowOffset = followOffset;
        }
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    uint32_t m_InputOffset{0};
    uint32_t m_CurrentBufferCxtOffset{0};
    uint32_t m_RemainingClean{0};
    uint32_t m_OutputFollowOffset{0};
    uint32_t m_SentinelFollow{NO_SENTINEL};

    std::atomic<OffsetType> m_OutPutOffset{};
    std::atomic<uint32_t> m_Remaining{0};
    std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;

    uint32_t const m_MemorySize;
    std::byte *const m_Memory;
};

#endif //FUNCTIONQUEUE_BUFFERQUEUE_MCSP_H
