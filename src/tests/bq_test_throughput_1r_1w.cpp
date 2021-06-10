#include "../BufferQueue_MCSP.h"
#include "../BufferQueue_SCSP.h"
#include "util.h"

#include <cstring>
#include <thread>

using namespace util;

template<typename BufferQueue>
void benchmark_throughput_fixed_size(std::string_view name, BufferQueue &bufferQueue, uint32_t buffer_size,
                                     uint32_t buffer_count) noexcept {
    std::jthread reader_thread{[&bufferQueue, name, buffer_count] {
        Timer timer{name};
        for (auto count = buffer_count; --count;) {
            while (!bufferQueue.reserve()) std::this_thread::yield();

            bufferQueue.consume([](auto, auto) {});
        }
    }};

    std::jthread writer_thread{[&bufferQueue, buffer_count, buffer_size] {
        for (auto count = buffer_count; count;) {
            bufferQueue.allocate_and_release(buffer_size, [&count](auto buff, auto size) {
                std::memset(buff, 2, size);
                --count;
                return size;
            });
        }
    }};
}

int main(int argc, char **argv) {
    if (argc == 1) {
        println("usage : ./bq_test_throughput_1r_1w <buffer_size> <fixed_chunk_size> <count> <min_chunk_size> <max_chunk_size> <count>");
    }

    auto const buffer_size = static_cast<size_t>(
            [&] { return (argc >= 2) ? atof(argv[1]) : 100000 / 1024.0 / 1024.0; }() * 1024 * 1024);

    println("using buffer of size :", buffer_size);

    size_t const chunk_size = [&] { return (argc >= 3) ? atol(argv[2]) : 1'500; }();
    println("chunk size for fixed buffer test :", chunk_size);

    size_t const fix_count = [&] { return (argc >= 3) ? atol(argv[3]) : 1'000'000; }();
    println("count for fixed size buffer test :", fix_count);

    size_t const min_chunk_size = [&] { return (argc >= 3) ? atol(argv[4]) : 500; }();
    println("min chunk size for variable size buffer test :", min_chunk_size);

    size_t const max_chunk_size = [&] { return (argc >= 3) ? atol(argv[5]) : 1'500; }();
    println("max chunk size for variable size buffer test :", max_chunk_size);

    size_t const var_count = [&] { return (argc >= 3) ? atol(argv[6]) : 1'000'000; }();
    println("count for variable size buffer test :", var_count);

    auto const buffer = std::make_unique<uint8_t[]>(buffer_size);
    BufferQueue_SCSP<false, false, alignof(size_t)> bufferQueueScsp{buffer.get(), buffer_size};
    BufferQueue_MCSP<false, false, alignof(size_t)> bufferQueueMcsp{buffer.get(), buffer_size};

    benchmark_throughput_fixed_size("buffer queue SCSP", bufferQueueScsp, chunk_size, fix_count);
    benchmark_throughput_fixed_size("buffer queue MCSP", bufferQueueMcsp, chunk_size, fix_count);
}
