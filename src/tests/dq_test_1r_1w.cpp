#include "../BufferQueue_MCSP.h"
#include "../BufferQueue_SCSP.h"
#include "util.h"
#include <boost/container_hash/hash.hpp>
#include <span>
#include <thread>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

using util::Random;
using util::StartFlag;
using util::Timer;

//using BufferQueue = BufferQueue_SCSP<false, false, alignof(size_t)>;
using BufferQueue = BufferQueue_MCSP<false, alignof(size_t)>;

void test_buffer_queue(BufferQueue &buffer_queue, Random<> &rng, size_t functions) noexcept;

int main(int argc, char **argv) {
    if (argc == 1) { fmt::print("usage : ./dq_test_1r_1w <buffer_size> <seed> <buffers>\n"); }

    size_t const buffer_size = [&] { return (argc >= 2) ? atof(argv[1]) : 10000 / 1024.0 / 1024.0; }() * 1024 * 1024;

    fmt::print("buffer size : {}\n", buffer_size);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    fmt::print("seed : {}\n", seed);

    size_t const buffers = [&] { return (argc >= 4) ? atol(argv[3]) : 1000000; }();
    fmt::print("buffers : {}\n", buffers);

    auto const buffer = std::make_unique<std::byte[]>(buffer_size);
    BufferQueue bufferQueue{buffer.get(), buffer_size};

    Random<> rng{seed};
    test_buffer_queue(bufferQueue, rng, buffers);
}

void test_buffer_queue(BufferQueue &buffer_queue, Random<> &rng, size_t buffers) noexcept {
    StartFlag start_flag;

    std::jthread reader{[&, buffers]() mutable {
        start_flag.wait();
        size_t total_bytes{0};
        size_t hash{0};
        {
            Timer timer{"reader"};
            while (buffers) {
                /*if (buffer_queue.reserve()) {
                    buffer_queue.consume([&hash, &total_bytes](std::span<std::byte> buffer) {
                        total_bytes += buffer.size();
                        boost::hash_range(hash, buffer.begin(), buffer.end());
                    });

                    --buffers;
                } else
                    std::this_thread::yield();*/

                if (buffer_queue.consume([&hash, &total_bytes](std::span<std::byte> buffer) {
                        total_bytes += buffer.size();
                        boost::hash_range(hash, buffer.begin(), buffer.end());
                    }))
                    --buffers;
                else
                    std::this_thread::yield();
            }
        }
        fmt::print("reader hash : {}, bytes read : {}\n", hash, total_bytes);
    }};

    std::jthread writer{[&, buffers] {
        start_flag.wait();
        auto func = buffers;
        size_t total_bytes{0};
        constexpr uint32_t min_buffer_size{3000};
        while (func) {
            buffer_queue.allocate_and_release(min_buffer_size, [&](std::span<std::byte> buffer) mutable {
                auto const fill_bytes{rng.getRand<uint32_t>(10, min_buffer_size)};
                total_bytes += fill_bytes;
                std::span const data{reinterpret_cast<char *>(buffer.data()), fill_bytes};
                rng.setRand(std::numeric_limits<char>::min(), std::numeric_limits<char>::max(), data);
                --func;
                return fill_bytes;
            });
        }
        fmt::print("bytes written : {}\n", total_bytes);
    }};

    start_flag.start();
}
