//
// Created by tekinas on 3/19/21.
//

#ifndef FUNCTIONQUEUE_FUNCTIONQUEUE_SCSP_H
#define FUNCTIONQUEUE_FUNCTIONQUEUE_SCSP_H

#include <atomic>
#include <cstring>
#include <cstddef>
#include <functional>
#include <cstdio>

template<typename T, bool readProtected, bool writeProtected>
class FunctionQueue_SCSP {
};

template<typename R, typename ...Args, bool readProtected, bool writeProtected>
class FunctionQueue_SCSP<R(Args...), readProtected, writeProtected> {
private:

    using InvokeAndDestroy = R(*)(void *data, Args...) noexcept;

    class Null {
    };

    struct FunctionContext {
        uint32_t const fp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        FunctionContext(InvokeAndDestroy fp, uint16_t obj_offset, uint16_t stride) noexcept:
                fp_offset{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fp) - fp_base)},
                obj_offset{obj_offset},
                stride{stride} {}
    };

    template<typename Callable>
    struct Storage {
        FunctionContext *const fp_ptr{};
        Callable *const obj_ptr{};

        Storage() noexcept = default;

        Storage(void *fp_ptr, void *obj_ptr) noexcept: fp_ptr{static_cast<FunctionContext *>(fp_ptr)},
                                                       obj_ptr{static_cast<Callable *>(obj_ptr)} {}

        explicit operator bool() const noexcept {
            return fp_ptr && obj_ptr;
        }

        [[nodiscard]] uint16_t getObjOffset() const noexcept {
            return reinterpret_cast<std::byte *>(obj_ptr) - reinterpret_cast<std::byte *>(fp_ptr);
        }

        [[nodiscard]] uint16_t getStride() const noexcept {
            return getObjOffset() + sizeof(Callable);
        }
    };

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
            functor(args...);
            std::invoke(*functor_ptr, args...);
            std::destroy_at(functor_ptr);
        } else {
            auto &&result{std::invoke(*functor_ptr, args...)};
            std::destroy_at(functor_ptr);
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename T>
    Storage<T> getMemory() noexcept {
        auto getAlignedStorage = [](void *buffer, size_t size) noexcept -> Storage<T> {
            auto const fp_storage = align<FunctionContext>(buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(FunctionContext);
            size -= sizeof(FunctionContext);
            auto const callable_storage = align<T>(buffer, size);
            return {fp_storage, callable_storage};
        };

        auto incr_input_offset = [&](auto &storage) noexcept {
            m_InputOffset.store(reinterpret_cast<std::byte *>(storage.obj_ptr) - m_Memory + sizeof(T),
                                std::memory_order::relaxed);
        };

        auto const remaining = m_Remaining.load(std::memory_order_acquire);
        auto const input_offset = m_InputOffset.load(std::memory_order_relaxed);
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);

        auto const search_ahead = (input_offset > output_offset) || (input_offset == output_offset && !remaining);

        if (size_t const buffer_size = m_MemorySize - input_offset;
                search_ahead && buffer_size) {
            if (auto storage = getAlignedStorage(m_Memory + input_offset, buffer_size)) {
                incr_input_offset(storage);
                return storage;
            }
        }

        {
            auto const mem = search_ahead ? 0 : input_offset;
            if (size_t const buffer_size = output_offset - mem) {
                if (auto storage = getAlignedStorage(m_Memory + mem, buffer_size)) {
                    if (search_ahead)
                        new(align<FunctionContext>(m_Memory + input_offset))
                                uint32_t{std::numeric_limits<uint32_t>::max()};
                    incr_input_offset(storage);
                    return storage;
                }
            }
        }

        return {};
    }

public:
    FunctionQueue_SCSP(void *memory, std::size_t size) noexcept: m_Memory{static_cast<std::byte *const>(memory)},
                                                                 m_MemorySize{static_cast<uint32_t>(size)},
                                                                 m_InputOffset{0},
                                                                 m_OutPutOffset{0},
                                                                 m_Remaining{0} {
        memset(m_Memory, 0, m_MemorySize);
    }

    explicit operator bool() noexcept {
        if constexpr (readProtected) {
            if (m_ReadFlag.test_and_set(std::memory_order_relaxed)) return false;
            if (!m_Remaining.load(std::memory_order_acquire)) {
                m_ReadFlag.clear(std::memory_order_relaxed);
                return false;
            } else return true;
        } else return m_Remaining.load(std::memory_order_acquire);
    }

    R callAndPop(Args...args) noexcept {
        auto const output_offset = m_OutPutOffset.load(std::memory_order_relaxed);
        auto functionCxtPtr = align<FunctionContext>(m_Memory + output_offset);

        if (functionCxtPtr->fp_offset == std::numeric_limits<uint32_t>::max())
            functionCxtPtr = align<FunctionContext>(m_Memory);

        auto incr_output_offset_decr_rem = [&] {
            m_OutPutOffset.store(reinterpret_cast<std::byte *>(functionCxtPtr) - m_Memory + functionCxtPtr->stride,
                                 std::memory_order_relaxed);

            if constexpr (readProtected) {
                m_Remaining.fetch_sub(1, std::memory_order_relaxed);
                m_ReadFlag.clear(std::memory_order_release);
            } else
                m_Remaining.fetch_sub(1, std::memory_order_release);
        };

        if constexpr (std::is_same_v<void, R>) {
            reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxtPtr->fp_offset)(
                    reinterpret_cast<std::byte *>(functionCxtPtr) + functionCxtPtr->obj_offset, args...);
            incr_output_offset_decr_rem();
        } else {
            auto &&result{reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxtPtr->fp_offset)(
                    reinterpret_cast<std::byte *>(functionCxtPtr) + functionCxtPtr->obj_offset, args...)};
            incr_output_offset_decr_rem();
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        if constexpr (writeProtected) {
            if (!m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        using Callable = std::decay_t<T>;

        auto const storage = getMemory<Callable>();
        if (!storage) {
            if constexpr (writeProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return false;
        }

        std::construct_at(storage.fp_ptr, invokeAndDestroy<Callable>, storage.getObjOffset(), storage.getStride());
        std::construct_at(storage.obj_ptr, std::forward<T>(function));

        m_Remaining.fetch_add(1, std::memory_order_release);

        if constexpr (writeProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

private:
    std::atomic<uint32_t> m_InputOffset;
    std::atomic<uint32_t> m_OutPutOffset;
    std::atomic<uint32_t> m_Remaining;

    [[no_unique_address]] std::conditional_t<writeProtected, std::atomic_flag, Null> m_WriteFlag;
    [[no_unique_address]] std::conditional_t<readProtected, std::atomic_flag, Null> m_ReadFlag;

    uint32_t m_MemorySize;
    std::byte *const m_Memory;

    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_same_v<R, void>) {
            return R{};
        }
    }

    static inline const uintptr_t fp_base = reinterpret_cast<uintptr_t>(&invokeAndDestroy<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);
};


#endif //FUNCTIONQUEUE_FUNCTIONQUEUE_SCSP_H
