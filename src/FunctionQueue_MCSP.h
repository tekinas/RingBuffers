#ifndef FUNCTIONQUEUE_MCSP
#define FUNCTIONQUEUE_MCSP

#include <atomic>
#include <cstring>
#include <cstddef>
#include <limits>
#include <memory>
#include <functional>
#include <mutex>

template<typename T, bool isWriteProtected, bool isIndexed = false, bool destroyNonInvoked = true>
class FunctionQueue_MCSP {
};

template<typename R, typename ...Args, bool isWriteProtected, bool isIndexed, bool destroyNonInvoked>
class FunctionQueue_MCSP<R(Args...), isWriteProtected, isIndexed, destroyNonInvoked> {
private:

    using InvokeAndDestroy = R(*)(void *obj_ptr, Args...) noexcept;
    using Destroy = void (*)(void *obj_ptr) noexcept;

    class Null {
    public:
        template<typename ...T>
        explicit Null(T &&...) noexcept {}
    };

    struct IndexedOffset {
    private:
        uint32_t offset;
        uint32_t index;

        IndexedOffset(uint32_t offset, uint32_t index) noexcept: offset{offset}, index{index} {}

    public:
        IndexedOffset(uint32_t offset) noexcept: offset{offset}, index{0} {}

        explicit operator uint32_t() const noexcept { return offset; }

        inline IndexedOffset getNext(uint32_t _offset) const noexcept {
            return {_offset, index + 1};
        }
    };

    using OffsetType = std::conditional_t<isIndexed, IndexedOffset, uint32_t>;

    template<typename>
    class Type {
    };

    struct Storage;

    struct FunctionContext {
    private:
        std::atomic<uint32_t> fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Null> destroyFp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        friend class Storage;

    public:
        inline auto getReadData() noexcept {
            return std::tuple{reinterpret_cast<InvokeAndDestroy>(fp_offset.load(std::memory_order_relaxed) + fp_base),
                              reinterpret_cast<std::byte *>(this) + obj_offset, getNextAddr()};
        }

        inline auto getDestroyData() noexcept {
            return std::tuple{reinterpret_cast<Destroy>(destroyFp_offset + fp_base),
                              reinterpret_cast<std::byte *>(this) + obj_offset, getNextAddr()};
        }

        inline auto getNextAddr() noexcept { return reinterpret_cast<std::byte *>(this) + stride; }

        inline void free() noexcept {
            fp_offset.store(0, std::memory_order_release);
        }

        [[nodiscard]] inline bool isFree() const noexcept {
            return fp_offset.load(std::memory_order_acquire) == 0;
        }

    private:
        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept:
                fp_offset{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(invokeAndDestroy < Callable > ) - fp_base)},
                destroyFp_offset{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destroy < Callable > ) - fp_base)},
                obj_offset{obj_offset},
                stride{stride} {}
    };

    struct Storage {
        FunctionContext *const fp_ptr{};
        void *const obj_ptr{};

        Storage() noexcept = default;

        Storage(void *fp_ptr, void *obj_ptr) noexcept: fp_ptr{static_cast<FunctionContext *>(fp_ptr)},
                                                       obj_ptr{obj_ptr} {}

        inline explicit operator bool() const noexcept {
            return fp_ptr && obj_ptr;
        }

        template<typename Callable, typename... CArgs>
        inline void construct_callable(CArgs &&... args) const noexcept {
            new(fp_ptr) FunctionContext{Type<Callable>{}, getObjOffset(),
                                        static_cast<uint16_t>(getObjOffset() + sizeof(Callable))};
            new(obj_ptr) Callable{std::forward<CArgs>(args)...};
        }

    private:
        [[nodiscard]] inline uint16_t getObjOffset() const noexcept {
            return reinterpret_cast<std::byte *>(obj_ptr) - reinterpret_cast<std::byte *>(fp_ptr);
        }
    };

public:
    FunctionQueue_MCSP(void *memory, std::size_t size) noexcept: m_Buffer{static_cast<std::byte *>(memory)},
                                                                 m_BufferSize{static_cast<uint32_t>(size)} {
        if constexpr (isWriteProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        memset(m_Buffer, 0, m_BufferSize);
    }

    auto buffer_size() const noexcept { return m_BufferSize; }

    ~FunctionQueue_MCSP() noexcept {
        if constexpr (destroyNonInvoked) {
            auto remaining = m_Remaining.load(std::memory_order_acquire);

            if (!remaining) return;

            auto output_pos = m_OutputFollowOffset;

            auto destroyAndForward = [&](auto functionCxt) noexcept {
                auto const[dfp, objPtr, nextAddr] = functionCxt->getDestroyData();
                dfp(objPtr);
                output_pos = nextAddr - m_Buffer;
            };

            if (auto const sentinel = m_SentinelRead.load(std::memory_order_relaxed); sentinel != NO_SENTINEL) {
                while (output_pos != sentinel) {
                    auto const functionCxt = align<FunctionContext>(m_Buffer + output_pos);
                    if (!functionCxt->isFree())
                        destroyAndForward(functionCxt);
                    --remaining;
                }
                output_pos = 0;
            }

            while (remaining) {
                auto const functionCxt = align<FunctionContext>(m_Buffer + output_pos);
                if (!functionCxt->isFree())
                    destroyAndForward(functionCxt);
                --remaining;
            }
        }
    }

    inline bool reserve_function() noexcept {
        uint32_t rem = m_Remaining.load(std::memory_order_relaxed);
        if (!rem) return false;

        while (rem &&
               !m_Remaining.compare_exchange_weak(rem, rem - 1, std::memory_order_acquire, std::memory_order_relaxed));
        return rem;
    }

    inline R call_and_pop(Args...args) noexcept {
        FunctionContext *functionCxt;
        auto out_pos = m_OutPutOffset.load(std::memory_order::relaxed);
        bool found_sentinel;
        while (!m_OutPutOffset.compare_exchange_weak(out_pos, [&, out_pos] {
            if ((found_sentinel = (uint32_t{out_pos} == m_SentinelRead.load(std::memory_order_relaxed)))) {
                functionCxt = align<FunctionContext>(m_Buffer);
            } else functionCxt = align<FunctionContext>(m_Buffer + uint32_t{out_pos});

            auto const nextOffset = functionCxt->getNextAddr() - m_Buffer;
            if constexpr (isIndexed) { return out_pos.getNext(nextOffset); }
            else return nextOffset;
        }(), std::memory_order_relaxed, std::memory_order_relaxed));

        auto const[invokeAndDestroyFP, objPtr, nextAddr] = functionCxt->getReadData();

        if constexpr (std::is_same_v<void, R>) {
            invokeAndDestroyFP(objPtr, args...);
            functionCxt->free();
            if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
        } else {
            auto &&result{invokeAndDestroyFP(objPtr, args...)};
            functionCxt->free();
            if (found_sentinel) m_SentinelRead.store(NO_SENTINEL, std::memory_order_relaxed);
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename T>
    inline bool push_back(T &&function) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        using Callable = std::decay_t<T>;

        auto const storage = getMemory(alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return false;
        }

        storage.template construct_callable<Callable>(std::forward<T>(function));

        m_Remaining.fetch_add(1, std::memory_order_release);

        ++m_RemainingClean;
        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

    template<typename Callable, typename ...CArgs>
    inline bool emplace_back(CArgs &&...args) noexcept {
        if constexpr (isWriteProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const storage = getMemory(alignof(Callable), sizeof(Callable));
        if (!storage) {
            if constexpr (isWriteProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return false;
        }

        storage.template construct_callable<Callable>(std::forward<CArgs>(args)...);

        m_Remaining.fetch_add(1, std::memory_order_release);

        ++m_RemainingClean;
        if constexpr (isWriteProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

    [[nodiscard]] inline uint32_t size() const noexcept { return m_Remaining.load(std::memory_order_relaxed); }

private:

    template<typename T>
    static constexpr inline T *align(void *ptr) noexcept {
        return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(ptr) - 1u + alignof(T)) & -alignof(T));
    }

    template<typename T>
    static inline void *
    align(void *&ptr, size_t &space) noexcept {
        const auto intptr = reinterpret_cast<uintptr_t>(ptr);
        const auto aligned = (intptr - 1u + alignof(T)) & -alignof(T);
        const auto diff = aligned - intptr;
        if ((sizeof(T) + diff) > space)
            return nullptr;
        else {
            space -= diff;
            return ptr = reinterpret_cast<void *>(aligned);
        }
    }

    template<typename Callable>
    static R invokeAndDestroy(void *data, Args... args) noexcept {
        auto const functor_ptr = static_cast<Callable *>(data);
        if constexpr (std::is_same_v<R, void>) {
            std::invoke(*functor_ptr, args...);
            std::destroy_at(functor_ptr);
        } else {
            auto &&result{std::invoke(*functor_ptr, args...)};
            std::destroy_at(functor_ptr);
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename Callable>
    static void destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
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
                } else if (auto const functionCxt = align<FunctionContext>(m_Buffer + followOffset);
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

    inline Storage getMemory(size_t obj_align, size_t obj_size) noexcept {
        auto getAlignedStorage = [obj_align, obj_size](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = std::align(alignof(FunctionContext), sizeof(FunctionContext), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(FunctionContext);
            size -= sizeof(FunctionContext);
            auto const callable_storage = std::align(obj_align, obj_size, buffer, size);
            return {fp_storage, callable_storage};
        };

        auto incr_input_offset = [&](auto &storage) noexcept {
            m_InputOffset = reinterpret_cast<std::byte *>(storage.obj_ptr) - m_Buffer + obj_size;
        };

        auto const remaining = m_RemainingClean;
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutputFollowOffset;

        auto const search_ahead = (input_offset > output_offset) || (input_offset == output_offset && !remaining);

        if (size_t const buffer_size = m_BufferSize - input_offset;
                search_ahead && buffer_size) {
            if (auto storage = getAlignedStorage(m_Buffer + input_offset, buffer_size)) {
                incr_input_offset(storage);
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
                    incr_input_offset(storage);
                    return storage;
                }
            }
        }

        cleanMemory();

        return {};
    }

private:
    static constexpr uint32_t NO_SENTINEL{std::numeric_limits<uint32_t>::max()};

    uint32_t m_InputOffset{0};
    uint32_t m_RemainingClean{0};
    uint32_t m_OutputFollowOffset{0};
    uint32_t m_SentinelFollow{NO_SENTINEL};

    std::atomic<uint32_t> m_Remaining{0};
    std::atomic<OffsetType> m_OutPutOffset{0};
    std::atomic<uint32_t> m_SentinelRead{NO_SENTINEL};

    [[no_unique_address]] std::conditional_t<isWriteProtected, std::atomic_flag, Null> m_WriteFlag;

    uint32_t const m_BufferSize;
    std::byte *const m_Buffer;

    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_same_v<R, void>) {
            return R{};
        }
    }

    static inline const uintptr_t fp_base = reinterpret_cast<uintptr_t>(&invokeAndDestroy<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);
};

#endif //FUNCTIONQUEUE_MCSP
