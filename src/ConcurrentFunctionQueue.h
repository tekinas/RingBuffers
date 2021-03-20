#ifndef FUNCTIONQUEUE_ConcurrentFunctionQueue_H
#define FUNCTIONQUEUE_ConcurrentFunctionQueue_H

#include <atomic>
#include <type_traits>
#include <tuple>
#include <cassert>
#include <iostream>

#include <boost/lockfree/queue.hpp>
#include <tbb/concurrent_hash_map.h>

#include <absl/hash/hash.h>

template<typename FunctionSignature>
class ConcurrentFunctionQueue {
};

struct MemBlock {
private:
    uint32_t offset /*: 24*/ {};
    uint32_t size /*: 8*/ {};


public:
    MemBlock() noexcept = default;

    inline void set(uint32_t memOffset, uint32_t objOffset, size_t objSize) noexcept {
        offset = memOffset;
        size = objOffset - memOffset + objSize;
    }

    [[nodiscard]] bool isSentinel() const noexcept { return !size; }

    static constexpr MemBlock Sentinel(uint32_t offset) noexcept {
        MemBlock sentinelBlock;
        sentinelBlock.offset = offset;
        sentinelBlock.size = 0;
        return sentinelBlock;
    }

    static constexpr MemBlock NullBlock() noexcept {
        MemBlock nullBlock;
        nullBlock.offset = 0;
        nullBlock.size = 0;
        return nullBlock;
    }

    [[nodiscard]] auto getOffset() const noexcept { return offset; }

    [[nodiscard]] auto getSize() const noexcept { return size; }

    friend bool operator==(const MemBlock &lhs, const MemBlock &rhs) noexcept {
        return lhs.offset == rhs.offset && lhs.size == rhs.size;
    }

    friend bool operator!=(const MemBlock &lhs, const MemBlock &rhs) noexcept {
        return !(lhs == rhs);
    }
};

class FreeMemSet {
private:
    using AtomicMemBlock = std::atomic<MemBlock>;

    std::unique_ptr<AtomicMemBlock[]> m_Array;
    size_t const m_ArraySize;
    std::atomic<uint32_t> m_CurrentPos;
    std::atomic<size_t> m_Count;
    std::atomic_flag m_CleanMemFlag;

public:
    explicit FreeMemSet(size_t size) : m_Array{std::make_unique<AtomicMemBlock[]>(size)}, m_ArraySize{size}, m_Count{} {
        memset(m_Array.get(), 0, size * sizeof(AtomicMemBlock));
    }

    [[nodiscard]] auto getCurrentPos() const noexcept { return m_CurrentPos.load(std::memory_order_acquire); }


    void addSentinel(uint32_t sentnelOffset) {
        if (sentnelOffset == getCurrentPos()) {
            while (m_CleanMemFlag.test_and_set(std::memory_order_acquire));
            m_CurrentPos.store(0, std::memory_order_release);
            m_CleanMemFlag.clear(std::memory_order_release);
        } else insert(MemBlock::Sentinel(sentnelOffset));
    }

    void freeMem(MemBlock const &freeBlock, std::atomic<size_t> &remaining) noexcept {
        if (freeBlock.getOffset() == getCurrentPos() /*!m_CleanMemFlag.test_and_set(std::memory_order_acquire)*/) {
            while (m_CleanMemFlag.test_and_set(std::memory_order_acquire));

            uint32_t nextOffset{getCurrentPos()};
            size_t cleaned{0};

            if (nextOffset == freeBlock.getOffset()) {
                if (freeBlock.isSentinel()) {
                    nextOffset = 0;
                } else {
                    nextOffset = freeBlock.getOffset() + freeBlock.getSize();
                    ++cleaned;
                }
            } else {
                insert(freeBlock);
            }

            if (m_Count.load(std::memory_order_acquire)) {
                MemBlock nextBlock = findAndClear(nextOffset);

                while (nextBlock != MemBlock::NullBlock()) {
                    if (nextBlock.isSentinel()) {
                        nextOffset = 0;
                    } else {
                        nextOffset = nextBlock.getOffset() + nextBlock.getSize();
                        ++cleaned;
                    }

                    nextBlock = findAndClear(nextOffset);
                }
            }

            if (cleaned) remaining.fetch_sub(cleaned, std::memory_order_relaxed);
            m_CurrentPos.store(nextOffset, std::memory_order_release);
            m_CleanMemFlag.clear(std::memory_order_release);

        } else {
            insert(freeBlock);
        }
    }

    void cleanMemory(std::atomic<size_t> &remaining) noexcept {
        if (m_Count.load(std::memory_order_acquire) && !m_CleanMemFlag.test_and_set(std::memory_order_acquire)) {
            uint32_t nextOffset{getCurrentPos()};
            MemBlock nextBlock = findAndClear(nextOffset);
            size_t cleaned{0};

            while (nextBlock != MemBlock::NullBlock()) {
                if (nextBlock.isSentinel()) {
                    nextOffset = 0;
                } else {
                    nextOffset = nextBlock.getOffset() + nextBlock.getSize();
                    ++cleaned;
                }

                nextBlock = findAndClear(nextOffset);
            }

            if (cleaned) remaining.fetch_sub(cleaned, std::memory_order_relaxed);
            m_CurrentPos.store(nextOffset, std::memory_order_release);
            m_CleanMemFlag.clear(std::memory_order_release);
        }
    }

private:
    void insert(MemBlock const &freeBlock) noexcept {
        for (size_t i = 0; i != m_ArraySize; ++i) {
            if (auto mblock = m_Array[i].load(std::memory_order_relaxed); mblock == MemBlock::NullBlock()) {
                if (m_Array[i].compare_exchange_strong(mblock, freeBlock)) {
                    m_Count.fetch_add(1, std::memory_order_release);
                    return;
                }
            }
        }
        std::cout << "error : not enough space for {" << freeBlock.getOffset() << "," << freeBlock.getSize()
                  << std::endl;
        exit(-1);
    }

    MemBlock findAndClear(uint32_t offset) noexcept {
        auto current_count = m_Count.load(std::memory_order_acquire);

        for (size_t i = 0; current_count && i != m_ArraySize; ++i) {
            if (auto mblock = m_Array[i].load(std::memory_order_relaxed); mblock != MemBlock::NullBlock()) {
                if (mblock.getOffset() == offset) {
                    if (m_Array[i].compare_exchange_strong(mblock, MemBlock::NullBlock())) {
                        m_Count.fetch_sub(1, std::memory_order_release);
                        return mblock;
                    }
                } else --current_count;
            }
        }

        return MemBlock::NullBlock();
    }
};

template<typename R, typename ...Args>
class ConcurrentFunctionQueue<R(Args...)> {
private:

    struct FunctionCxt {
        uint32_t fp_offset{};
        MemBlock obj{};
    };

    struct HashedMemPos {
    private:
        uint32_t hash;
        uint32_t offset;

        HashedMemPos(size_t hash, uint32_t offset) : hash{static_cast<uint32_t>(hash)}, offset{offset} {}

    public:
        HashedMemPos() noexcept = default;

        template<typename H>
        friend H AbslHashValue(H h, const HashedMemPos &c) {
            return H::combine(std::move(h), c.hash, c.offset);
        }

        HashedMemPos getNext(uint32_t next_offset) const noexcept {
            return {absl::Hash<HashedMemPos>{}(*this), next_offset};
        }

        auto getOffset() const noexcept { return offset; }
    };

    std::atomic<size_t> m_RemainingRead;
    std::atomic<HashedMemPos> m_InputPos;
    std::atomic<size_t> m_Remaining;
    std::atomic<size_t> m_Writing;

    boost::lockfree::queue<FunctionCxt/*, boost::lockfree::capacity<100>*/> m_ReadFunctions;
    FreeMemSet m_FreePtrSet;

    std::byte *const m_Memory;
    size_t const m_MemorySize;

    template<typename Callable>
    static R invoke(void *data, Args... args) {
        auto &functor = *std::launder(align<Callable>(data));
        if constexpr (std::is_same_v<R, void>) {
            functor(args...);
            functor.~Callable();
        } else {
            auto &&result = functor(args...);
            functor.~Callable();
            return std::move(result);
        }
    }

    using InvokeAndDestroy = R(*)(void *data, Args...);

    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_same_v<R, void>) {
            return R{};
        }
    }

    static inline const uintptr_t fp_base = reinterpret_cast<uintptr_t>(&invoke<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);

    template<typename T>
    static constexpr inline T *align(void *ptr) noexcept {
        return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(ptr) - 1u + alignof(T)) & -alignof(T));
    }

    template<typename Callable>
    MemBlock getCallableStorage() noexcept {
        auto getAlignedStorage = [](void *ptr, size_t space) noexcept -> std::byte * {
            const auto intptr = reinterpret_cast<uintptr_t>(ptr);
            const auto aligned = (intptr - 1u + alignof(Callable)) & -alignof(Callable);
            const auto diff = aligned - intptr;
            if ((sizeof(Callable) + diff) > space)
                return nullptr;
            else
                return reinterpret_cast<std::byte *>(aligned);
        };

        MemBlock memCxt;
        HashedMemPos input_mem = m_InputPos.load(std::memory_order_acquire);

        bool search_ahead;
        m_Writing.fetch_add(1, std::memory_order_relaxed);

        do {
            m_Writing.fetch_sub(1, std::memory_order_relaxed);

            memCxt = {};
            auto const out_pos = m_FreePtrSet.getCurrentPos();
            auto const rem = m_Remaining.load(std::memory_order_relaxed);

            auto const input_pos = input_mem.getOffset();

            if (out_pos == input_pos) {
                if (rem || m_Writing.load(std::memory_order_acquire)) {
                    m_FreePtrSet.cleanMemory(m_Remaining);
                    return {};
                }
            }

            search_ahead = input_pos >= out_pos;

            if (size_t const buffer_size = m_MemorySize - input_pos; search_ahead && buffer_size) {
                if (auto obj_buff = getAlignedStorage(m_Memory + input_pos, buffer_size)) {
                    search_ahead = false;
                    memCxt.set(input_pos, std::distance(m_Memory, obj_buff), sizeof(Callable));
                    m_Writing.fetch_add(1, std::memory_order_release);
                    continue;
                }
            }

            {
                auto const memOffset = search_ahead ? 0 : input_pos;
                if (size_t const buffer_size = out_pos - memOffset) {
                    if (auto obj_buff = getAlignedStorage(m_Memory + memOffset, buffer_size)) {
                        memCxt.set(memOffset, std::distance(m_Memory, obj_buff), sizeof(Callable));
                    }
                }
            }

            if (!memCxt.getSize()) {
                m_FreePtrSet.cleanMemory(m_Remaining);
                return {};
            }

            m_Writing.fetch_add(1, std::memory_order_release);

        } while (!m_InputPos.compare_exchange_weak(input_mem, input_mem.getNext(memCxt.getOffset() + memCxt.getSize()),
                                                   std::memory_order_release, std::memory_order_acquire));

        if (search_ahead) {
            m_FreePtrSet.addSentinel(input_mem.getOffset());
        }

        return memCxt;
    }

public:

    ConcurrentFunctionQueue(void *mem, size_t size) : m_Memory{static_cast<std::byte *const>(mem)}, m_MemorySize{size},
                                                      m_RemainingRead{0}, m_Remaining{0}, m_ReadFunctions{1000},
                                                      m_InputPos{}, m_FreePtrSet{50 * 1024 * 1024 / 8} {
        memset(m_Memory, 0, m_MemorySize);
    }

    explicit operator bool() {
        size_t rem = m_RemainingRead.load(std::memory_order_relaxed);
        if (!rem) return false;

        while (rem && !m_RemainingRead.compare_exchange_weak(rem, rem - 1, std::memory_order_acquire,
                                                             std::memory_order_relaxed));
        return rem;
    }

    inline R callAndPop(Args ... args) noexcept {
        FunctionCxt functionCxt;

        while (m_ReadFunctions.empty()) std::this_thread::yield();
        m_ReadFunctions.pop(functionCxt);

        if constexpr (std::is_same_v<R, void>) {
            reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(m_Memory + functionCxt.obj.getOffset(),
                                                                                args...);

            m_FreePtrSet.freeMem(functionCxt.obj, m_Remaining);

        } else {
            auto &&result = reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(
                    m_Memory + functionCxt.obj.getOffset(),
                    args...);

            m_FreePtrSet.freeMem(functionCxt.obj, m_Remaining);

            return std::move(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;

        MemBlock const memCxt = getCallableStorage<Callable>();

        if (!memCxt.getSize())
            return false;

        m_Remaining.fetch_add(1, std::memory_order_release);

        std::construct_at(align<Callable>(m_Memory + memCxt.getOffset()), std::forward<T>(function));

        m_ReadFunctions.push(
                {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&invoke<Callable> ) - fp_base), memCxt});

        m_RemainingRead.fetch_add(1, std::memory_order_release);

        m_Writing.fetch_sub(1, std::memory_order_release);

        return true;
    }

};

#endif //FUNCTIONQUEUE_ConcurrentFunctionQueue_H
