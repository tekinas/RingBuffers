//
// Created by tekinas on 3/24/21.
//

#ifndef FUNCTIONQUEUE_FunctionQueue_MCSP_1_1_H
#define FUNCTIONQUEUE_FunctionQueue_MCSP_1_1_H

template<typename T, bool writeProtected, bool destroyNonInvoked = true>
class FunctionQueue_MCSP {
};

template<typename R, typename ...Args, bool writeProtected, bool destroyNonInvoked>
class FunctionQueue_MCSP<R(Args...), writeProtected, destroyNonInvoked> {
private:

    using InvokeAndDestroy = R(*)(void *obj_ptr, Args...) noexcept;

    class Null {
    public:
        template<typename ...T>
        explicit Null(T &&...) noexcept {}
    };

    template<typename>
    class Type {
    };

    template<typename T>
    struct Storage;

    struct FunctionContext {
        std::atomic<uint32_t> fp_offset;
        [[no_unique_address]] std::conditional_t<destroyNonInvoked, uint32_t const, Null> destroyFp_offset;
        uint16_t const obj_offset;
        uint16_t const stride;

        template<typename U>
        friend
        class Storage;

    private:
        template<typename Callable>
        FunctionContext(Type<Callable>, uint16_t obj_offset, uint16_t stride) noexcept:
                fp_offset{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(invokeAndDestroy < Callable > ) - fp_base)},
                destroyFp_offset{static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destroy < Callable > ) - fp_base)},
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

        template<typename... CArgs>
        void construct_callable(CArgs &&... args) const noexcept {
            new(fp_ptr) FunctionContext{Type<Callable>{}, getObjOffset(), getStride()};
            new(obj_ptr) Callable{std::forward<CArgs>(args)...};
        }

    private:
        [[nodiscard]] uint16_t getObjOffset() const noexcept {
            return reinterpret_cast<std::byte *>(obj_ptr) - reinterpret_cast<std::byte *>(fp_ptr);
        }

        [[nodiscard]] uint16_t getStride() const noexcept {
            return getObjOffset() + sizeof(Callable);
        }
    };

public:
    FunctionQueue_MCSP(void *memory, std::size_t size) noexcept: m_Memory{static_cast<std::byte *const>(memory)},
                                                                 m_MemorySize{static_cast<uint32_t>(size)} {
        if constexpr (writeProtected) m_WriteFlag.clear(std::memory_order_relaxed);
        memset(m_Memory, 0, m_MemorySize);
    }

    ~FunctionQueue_MCSP() noexcept {
        if constexpr (destroyNonInvoked) {
            using Destroy = void (*)(void *obj_ptr) noexcept;

            auto remaining = m_Remaining.load(std::memory_order_acquire);

            if (!remaining) return;

            auto const input_pos = m_InputOffset;
            auto output_pos = m_OutputFollowOffset;

            auto destroyAndForward = [&](auto functionCxt) noexcept {
                reinterpret_cast<Destroy>(fp_base + functionCxt->destroyFp_offset)(
                        reinterpret_cast<std::byte *>(functionCxt) + functionCxt->obj_offset);
                output_pos = reinterpret_cast<std::byte *>(functionCxt) - m_Memory + functionCxt->stride;
            };

            if (output_pos >= input_pos) {
                while (remaining) {
                    auto const functionCxtPtr = align<FunctionContext>(m_Memory + output_pos);
                    if (auto const fp_offset = functionCxtPtr->fp_offset.load(std::memory_order_acquire);
                            fp_offset != std::numeric_limits<uint32_t>::max()) {
                        if (fp_offset)
                            destroyAndForward(functionCxtPtr);
                        --remaining;
                    } else break;
                }
            }

            while (remaining) {
                auto const functionCxtPtr = align<FunctionContext>(m_Memory + output_pos);
                if (functionCxtPtr->fp_offset.load(std::memory_order_acquire))
                    destroyAndForward(functionCxtPtr);
                --remaining;
            }
        }
    }

    explicit operator bool() noexcept {
        uint32_t rem = m_Remaining.load(std::memory_order_relaxed);
        if (!rem) return false;

        while (rem &&
               !m_Remaining.compare_exchange_weak(rem, rem - 1, std::memory_order_acquire, std::memory_order_relaxed));
        return rem;
    }

    R callAndPop(Args...args) noexcept {
        FunctionContext *functionCxt;
        auto out_pos = m_OutPutOffset.load(std::memory_order::relaxed);
        while (!m_OutPutOffset.compare_exchange_weak(out_pos, [&, out_pos] {
            functionCxt = align<FunctionContext>(m_Memory + out_pos);

            if (functionCxt->fp_offset.load(std::memory_order_relaxed) == std::numeric_limits<uint32_t>::max()) {
                functionCxt = align<FunctionContext>(m_Memory);
            }

            return reinterpret_cast<std::byte *>(functionCxt) - m_Memory + functionCxt->stride;
        }(), std::memory_order_relaxed, std::memory_order_relaxed));

        auto const invokeAndDestroyFP = reinterpret_cast<InvokeAndDestroy>(functionCxt->fp_offset + fp_base);
        auto const objPtr = reinterpret_cast<std::byte *>(functionCxt) + functionCxt->obj_offset;

        auto after_call = [=] {
            functionCxt->fp_offset.store(0, std::memory_order_release);
        };

        if constexpr (std::is_same_v<void, R>) {
            invokeAndDestroyFP(objPtr, args...);
            after_call();
        } else {
            auto &&result{invokeAndDestroyFP(objPtr, args...)};
            after_call();
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

        storage.construct_callable(std::forward<T>(function));

        m_Remaining.fetch_add(1, std::memory_order_release);

        ++m_RemainingClean;
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

        storage.construct_callable(std::forward<CArgs>(args)...);

        ++m_RemainingClean;
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

    void cleanMemory() noexcept {
        if (m_RemainingClean) {
            while (m_RemainingClean) {
                auto const functionCxt = align<FunctionContext>(m_Memory + m_OutputFollowOffset);
                auto const fp_offset = functionCxt->fp_offset.load(std::memory_order_acquire);
                if (fp_offset == 0) {
                    m_OutputFollowOffset = reinterpret_cast<std::byte *>(functionCxt) - m_Memory + functionCxt->stride;
                    --m_RemainingClean;
                } else if (fp_offset == std::numeric_limits<uint32_t>::max()) {
                    m_OutputFollowOffset = 0;
                } else break;
            }
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
            m_InputOffset = reinterpret_cast<std::byte *>(storage.obj_ptr) - m_Memory + sizeof(T);
        };

        auto const remaining = m_RemainingClean;
        auto const input_offset = m_InputOffset;
        auto const output_offset = m_OutputFollowOffset;

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
                                std::atomic<uint32_t>{std::numeric_limits<uint32_t>::max()};
                    incr_input_offset(storage);
                    return storage;
                }
            }
        }

        cleanMemory();

        return {};
    }

private:
    uint32_t m_InputOffset{0};
    uint32_t m_RemainingClean{0};
    uint32_t m_OutputFollowOffset{0};

    std::atomic<uint32_t> m_OutPutOffset{0};
    std::atomic<uint32_t> m_Remaining{0};

    [[no_unique_address]] std::conditional_t<writeProtected, std::atomic_flag, Null> m_WriteFlag;

    uint32_t const m_MemorySize;
    std::byte *const m_Memory;

    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_same_v<R, void>) {
            return R{};
        }
    }

    static inline const uintptr_t fp_base = reinterpret_cast<uintptr_t>(&invokeAndDestroy<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);
};

#endif //FUNCTIONQUEUE_FunctionQueue_MCSP_1_1_H
