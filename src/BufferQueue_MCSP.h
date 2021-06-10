#ifndef FUNCTIONQUEUE_BUFFERQUEUE_MCSP_H
#define FUNCTIONQUEUE_BUFFERQUEUE_MCSP_H

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

template <bool isWriteProtected, bool isIndexed,
          size_t buffer_align = sizeof(std::max_align_t)>
class BufferQueue_MCSP {

private:
  struct IndexedOffset {
  private:
    uint32_t offset{};
    uint32_t index{};

    IndexedOffset(uint32_t offset, uint32_t index) noexcept
        : offset{offset}, index{index} {}

  public:
    explicit IndexedOffset(uint32_t offset) noexcept
        : offset{offset}, index{0} {}

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
    uint32_t const stride;

    friend class Storage;

    DataContext(uint32_t buffer_size, uint32_t stride) noexcept
        : buffer_size{buffer_size}, stride{stride} {}

    explicit DataContext(uint32_t buffer_size) noexcept
        : buffer_size{buffer_size}, stride{static_cast<uint32_t>(
                                        align<std::byte, buffer_align>(this +
                                                                       1) -
                                        std::bit_cast<std::byte *>(this) +
                                        buffer_size)} {}

  public:
    inline std::pair<std::byte *, uint32_t> getBuffer() noexcept {
      return {align<std::byte, buffer_align>(this + 1),
              buffer_size.load(std::memory_order_relaxed)};
    }

    inline auto getNextAddr() noexcept {
      return std::bit_cast<std::byte *>(this) + stride;
    }

    inline void free() noexcept {
      buffer_size.store(0, std::memory_order_release);
    }

    [[nodiscard]] inline bool isFree() const noexcept {
      return buffer_size.load(std::memory_order_acquire) == 0;
    }
  };

  struct Storage {
    DataContext *const dc_ptr{};
    std::byte *const buffer{};
    uint32_t const avl_size{};

    Storage() noexcept = default;

    Storage(void *dc_ptr, void *buffer, uint32_t avl_size) noexcept
        : dc_ptr{static_cast<DataContext *>(dc_ptr)},
          buffer{static_cast<std::byte *>(buffer)}, avl_size{avl_size} {}

    inline explicit operator bool() const noexcept { return dc_ptr && buffer; }

    [[nodiscard]] inline auto
    createContext(uint32_t buffer_size) const noexcept {
      new (dc_ptr) DataContext{
          buffer_size,
          static_cast<uint32_t>(buffer - std::bit_cast<std::byte *>(dc_ptr) +
                                buffer_size)};
      return dc_ptr;
    }

    inline static auto createContext(void *addr,
                                     uint32_t buffer_size) noexcept {
      new (addr) DataContext{buffer_size};
      return static_cast<DataContext *>(addr);
    }
  };

  class Null {
  public:
    template <typename... T> explicit Null(T &&...) noexcept {}
  };

public:
  class Buffer {
  public:
    [[nodiscard]] inline std::pair<std::byte *, uint32_t> get() const noexcept {
      return dataContext->getBuffer();
    }

    Buffer(Buffer &&other) noexcept
        : dataContext{std::exchange(other.dataContext, nullptr)} {}

    Buffer &operator=(Buffer &&other) noexcept {
      this->~Buffer();
      dataContext = std::exchange(other.dataContext, nullptr);
      return *this;
    }

    ~Buffer() noexcept {
      if (dataContext)
        dataContext->free();
    }

  private:
    friend class BufferQueue_MCSP;

    explicit Buffer(DataContext *dataContext) noexcept
        : dataContext{dataContext} {}

    DataContext *dataContext;
  };

public:
  BufferQueue_MCSP(void *memory, std::size_t size) noexcept
      : m_Buffer{static_cast<std::byte *const>(memory)},
        m_BufferSize{static_cast<uint32_t>(size)} {
    if constexpr (isWriteProtected)
      m_WriteFlag.clear(std::memory_order_relaxed);
  }

  inline auto buffer_size() const noexcept { return m_BufferSize; }

  inline auto size() const noexcept {
    return m_Remaining.load(std::memory_order_relaxed);
  }

  void clear() noexcept {
    m_InputOffset = 0;
    m_CurrentBufferCxtOffset = 0;
    m_RemainingClean = 0;
    m_OutputFollowOffset = 0;
    m_SentinelFollow = 0;
    m_OutPutOffset.store(0, std::memory_order_relaxed);
    m_Remaining.store(0, std::memory_order_relaxed);
    m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);

    if constexpr (isWriteProtected)
      m_WriteFlag.clear(std::memory_order_relaxed);
  }

  inline bool reserve() const noexcept {
    uint32_t rem = m_Remaining.load(std::memory_order_relaxed);
    if (!rem)
      return false;

    while (rem && !m_Remaining.compare_exchange_weak(rem, rem - 1,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed))
      ;
    return rem;
  }

  template <typename F>
  inline decltype(auto) consume(F &&functor) const noexcept {
    DataContext *data_cxt;
    auto out_pos = m_OutPutOffset.load(std::memory_order::relaxed);
    bool found_sentinel;
    while (!m_OutPutOffset.compare_exchange_weak(
        out_pos,
        [&, out_pos] {
          if ((found_sentinel =
                   (uint32_t{out_pos} ==
                    m_SentinelRead.load(std::memory_order_relaxed)))) {
            data_cxt = align<DataContext>(m_Buffer);
          } else
            data_cxt = align<DataContext>(m_Buffer + uint32_t{out_pos});

          auto const nextOffset = data_cxt->getNextAddr() - m_Buffer;
          if constexpr (isIndexed) {
            return out_pos.getNext(nextOffset);
          } else
            return nextOffset;
        }(),
        std::memory_order_relaxed, std::memory_order_relaxed))
      ;

    auto const [buffer_data, buffer_size] = data_cxt->getBuffer();

    using ReturnType =
        decltype(functor(std::declval<std::byte *>(), uint32_t{}));
    if constexpr (std::is_same_v<void, ReturnType>) {
      std::forward<F>(functor)(buffer_data, buffer_size);
      data_cxt->free();
      if (found_sentinel)
        m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
    } else {
      auto &&result{std::forward<F>(functor)(buffer_data, buffer_size)};
      data_cxt->free();
      if (found_sentinel)
        m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
      return std::forward<decltype(result)>(result);
    }
  }

  inline auto consume() const noexcept {
    DataContext *data_cxt;
    auto out_pos = m_OutPutOffset.load(std::memory_order::relaxed);
    bool found_sentinel;
    while (!m_OutPutOffset.compare_exchange_weak(
        out_pos,
        [&, out_pos] {
          if ((found_sentinel =
                   (uint32_t{out_pos} ==
                    m_SentinelRead.load(std::memory_order_relaxed)))) {
            data_cxt = align<DataContext>(m_Buffer);
          } else
            data_cxt = align<DataContext>(m_Buffer + uint32_t{out_pos});

          auto const nextOffset = data_cxt->getNextAddr() - m_Buffer;
          if constexpr (isIndexed) {
            return out_pos.getNext(nextOffset);
          } else
            return nextOffset;
        }(),
        std::memory_order_relaxed, std::memory_order_relaxed))
      ;

    if (found_sentinel)
      m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);

    return Buffer{data_cxt};
  }

  inline std::pair<std::byte *, uint32_t>
  allocate(uint32_t buffer_size) noexcept {
    if constexpr (isWriteProtected) {
      if (m_WriteFlag.test_and_set(std::memory_order_acquire))
        return {nullptr, 0};
    }

    auto const storage = getMemory(buffer_size);
    if (!storage) {
      if constexpr (isWriteProtected) {
        m_WriteFlag.clear(std::memory_order_release);
      }
      return {nullptr, 0};
    }

    m_CurrentBufferCxtOffset =
        std::bit_cast<std::byte *>(storage.dc_ptr) - m_Buffer;

    return {storage.buffer, storage.avl_size};
  }

  void release(uint32_t bytes_used) noexcept {
    auto const dc_ptr =
        Storage::createContext(m_Buffer + m_CurrentBufferCxtOffset, bytes_used);
    m_InputOffset = dc_ptr->getNextAddr() - m_Buffer;
    m_Remaining.fetch_add(1, std::memory_order_release);
    ++m_RemainingClean;

    if constexpr (isWriteProtected) {
      m_WriteFlag.clear(std::memory_order_release);
    }
  }

  template <typename F>
  inline void allocate_and_release(uint32_t buffer_size, F &&functor) noexcept {
    if constexpr (isWriteProtected) {
      if (m_WriteFlag.test_and_set(std::memory_order_acquire))
        return;
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
    ++m_RemainingClean;

    if constexpr (isWriteProtected) {
      m_WriteFlag.clear(std::memory_order_release);
    }
  }

  inline uint32_t available_space() noexcept {
    auto getUsableSpace = [](void *buffer, size_t size) noexcept -> uint32_t {
      auto const fp_storage =
          std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
      buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
      size -= sizeof(DataContext);
      auto const callable_storage = std::align(buffer_align, 0, buffer, size);
      return fp_storage && callable_storage ? static_cast<uint32_t>(size) : 0;
    };

    cleanMemory();

    auto const remaining = m_RemainingClean;
    auto const input_offset = m_InputOffset;
    auto const output_offset = m_OutputFollowOffset;

    if ((input_offset > output_offset) ||
        (input_offset == output_offset && !remaining)) {
      return std::max(
          getUsableSpace(m_Buffer + input_offset, m_BufferSize - input_offset),
          getUsableSpace(m_Buffer, output_offset));
    } else if (input_offset < output_offset) {
      return getUsableSpace(m_Buffer + input_offset,
                            output_offset - input_offset);
    } else
      return 0;
  }

private:
  template <typename T, size_t _align = alignof(T)>
  static constexpr inline T *align(void *ptr) noexcept {
    return std::bit_cast<T *>((std::bit_cast<uintptr_t>(ptr) - 1u + _align) &
                              -_align);
  }

  inline Storage getMemory(uint32_t size) noexcept {
    auto getAlignedStorage =
        [buffer_size{size}](void *buffer, size_t size) noexcept -> Storage {
      auto const fp_storage =
          std::align(alignof(DataContext), sizeof(DataContext), buffer, size);
      buffer = static_cast<std::byte *>(buffer) + sizeof(DataContext);
      size -= sizeof(DataContext);
      auto const callable_storage =
          std::align(buffer_align, buffer_size, buffer, size);
      return {fp_storage, callable_storage, static_cast<uint32_t>(size)};
    };

    auto const remaining = m_RemainingClean;
    auto const input_offset = m_InputOffset;
    auto const output_offset = m_OutputFollowOffset;

    auto const search_ahead = (input_offset > output_offset) ||
                              (input_offset == output_offset && !remaining);

    if (size_t const buffer_size = m_BufferSize - input_offset;
        search_ahead && buffer_size) {
      if (auto storage =
              getAlignedStorage(m_Buffer + input_offset, buffer_size)) {
        return storage;
      }
    }

    {
      auto const mem = search_ahead ? 0 : input_offset;
      if (size_t const buffer_size = output_offset - mem) {
        if (auto storage = getAlignedStorage(m_Buffer + mem, buffer_size)) {
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
        } else if (auto const functionCxt =
                       align<DataContext>(m_Buffer + followOffset);
                   functionCxt->isFree()) {
          followOffset = functionCxt->getNextAddr() - m_Buffer;
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

  mutable std::atomic<OffsetType> m_OutPutOffset{0};
  mutable std::atomic<uint32_t> m_Remaining{0};
  mutable std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

  [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag,
                                           Null>
      m_WriteFlag;

  uint32_t const m_BufferSize;
  std::byte *const m_Buffer;
};

#endif // FUNCTIONQUEUE_BUFFERQUEUE_MCSP_H
