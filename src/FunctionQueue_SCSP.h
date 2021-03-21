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

template<typename T, bool readProtected, bool writeProtected, bool destroyNonInvoked = true>
class FunctionQueue_SCSP {
};

template<typename R, typename ...Args, bool readProtected, bool writeProtected, bool destroyNonInvoked>
class FunctionQueue_SCSP<R(Args...), readProtected, writeProtected, destroyNonInvoked> {
private:

    using InvokeAndDestroy = R(*)(void *obj_ptr, Args...) noexcept;

    class Null {
    public:
        template<typename ...T>
        explicit Null(T...) noexcept {}
    };

    template<typename>
    class Type {
    };

    struct FunctionContext {
        uint32_t const fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Null> destroyFp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept:
                fp_offset{
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(invokeAndDestroy < Callable > ) - fp_base)},
                destroyFp_offset{
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destroy < Callable > ) - fp_base)},
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

public:
    FunctionQueue_SCSP(void *memory, std::size_t size) noexcept: m_Memory{static_cast<std::byte *const>(memory)},
                                                                 m_MemorySize{static_cast<uint32_t>(size)},
                                                                 m_InputOffset{0},
                                                                 m_OutPutOffset{0},
                                                                 m_Remaining{0}, m_ReadFlag{false}, m_WriteFlag{false} {
        memset(m_Memory, 0, m_MemorySize);
    }

    ~FunctionQueue_SCSP() noexcept {
        if constexpr (destroyNonInvoked) {
            using Destroy = void (*)(void *obj_ptr) noexcept;

            auto remaining = m_Remaining.load(std::memory_order_acquire);

            if (!remaining) return;

            auto const input_pos = m_InputOffset.load(std::memory_order_relaxed);
            auto output_pos = m_OutPutOffset.load(std::memory_order_relaxed);

            auto destroyAndForward = [&](auto functionCxt) noexcept {
                reinterpret_cast<Destroy>(fp_base + functionCxt->destroyFp_offset)(
                        reinterpret_cast<std::byte *>(functionCxt) + functionCxt->obj_offset);
                output_pos = reinterpret_cast<std::byte *>(functionCxt) - m_Memory + functionCxt->stride;
            };

            if (output_pos >= input_pos) {
                while (remaining) {
                    if (auto const functionCxt = align<FunctionContext>(m_Memory + output_pos);
                            functionCxt->fp_offset != std::numeric_limits<uint32_t>::max()) {
                        destroyAndForward(functionCxt);
                        --remaining;
                    } else break;
                }
            }

            while (remaining) {
                auto const functionCxt = align<FunctionContext>(m_Memory + output_pos);
                destroyAndForward(functionCxt);
                --remaining;
            }
        }
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
        auto const[invokeAndDestroyFP, objPtr, nextOffset] = extractCallableData();

        auto incr_output_offset_decr_rem = [&, nextOffset{nextOffset}] {
            m_OutPutOffset.store(nextOffset, std::memory_order_relaxed);

            if constexpr (readProtected) {
                m_Remaining.fetch_sub(1, std::memory_order_relaxed);
                m_ReadFlag.clear(std::memory_order_release);
            } else
                m_Remaining.fetch_sub(1, std::memory_order_release);
        };

        if constexpr (std::is_same_v<void, R>) {
            invokeAndDestroyFP(objPtr, args...);
            incr_output_offset_decr_rem();
        } else {
            auto &&result{invokeAndDestroyFP(objPtr, args...)};
            incr_output_offset_decr_rem();
            return std::forward<decltype(result)>(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        if constexpr (writeProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        using Callable = std::decay_t<T>;

        auto const storage = getMemory<Callable>();
        if (!storage) {
            if constexpr (writeProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return false;
        }

        std::construct_at(storage.fp_ptr, Type<Callable>{}, storage.getObjOffset(), storage.getStride());
        std::construct_at(storage.obj_ptr, std::forward<T>(function));

        m_Remaining.fetch_add(1, std::memory_order_release);

        if constexpr (writeProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

    template<typename Callable, typename ...CArgs>
    bool emplace_back(Args &&...args) noexcept {
        if constexpr (writeProtected) {
            if (m_WriteFlag.test_and_set(std::memory_order_acquire)) return false;
        }

        auto const storage = getMemory<Callable>();
        if (!storage) {
            if constexpr (writeProtected) {
                m_WriteFlag.clear(std::memory_order_release);
            }
            return false;
        }

        std::construct_at(storage.fp_ptr, Type<Callable>{}, storage.getObjOffset(), storage.getStride());
        std::construct_at(storage.obj_ptr, std::forward<CArgs>(args)...);

        m_Remaining.fetch_add(1, std::memory_order_release);

        if constexpr (writeProtected) {
            m_WriteFlag.clear(std::memory_order_release);
        }

        return true;
    }

    [[nodiscard]] uint32_t size() const noexcept { return m_Remaining.load(std::memory_order_relaxed); }

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

    template<typename Callable>
    static R destroy(void *data) noexcept {
        std::destroy_at(static_cast<Callable *>(data));
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

    std::tuple<InvokeAndDestroy, void *, uint32_t> extractCallableData() const noexcept {
        auto functionCxtPtr = align<FunctionContext>(m_Memory + m_OutPutOffset.load(std::memory_order_relaxed));

        if (functionCxtPtr->fp_offset == std::numeric_limits<uint32_t>::max())
            functionCxtPtr = align<FunctionContext>(m_Memory);

        return {reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxtPtr->fp_offset),
                reinterpret_cast<std::byte *>(functionCxtPtr) + functionCxtPtr->obj_offset,
                reinterpret_cast<std::byte *>(functionCxtPtr) - m_Memory + functionCxtPtr->stride};
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
