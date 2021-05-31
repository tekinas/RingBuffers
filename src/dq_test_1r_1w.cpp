#include "BufferQueue_SCSP.h"
#include "BufferQueue_MCSP.h"
#include "util.h"
#include <thread>
#include <boost/container_hash/hash.hpp>
#include <span>

using namespace util;

//using BufferQueue = BufferQueue_SCSP<false, false, alignof(size_t)>;
using BufferQueue = BufferQueue_MCSP<false, false, alignof(size_t)>;

using RNG = Random<>;

void test_buffer_queue(BufferQueue &buffer_queue, RNG &rng, size_t functions) noexcept;

int main(int argc, char **argv) {
    if (argc == 1) {
        println("usage : ./dq_test_1r_1w <buffer_size> <seed> <buffers>");
    }

    size_t const buffer_size =
            [&] { return (argc >= 2) ? atof(argv[1]) : 10000 / 1024.0 / 1024.0; }() * 1024 * 1024;

    println("using buffer of size :", buffer_size);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const buffers = [&] { return (argc >= 4) ? atol(argv[3]) : 1000000; }();
    println("total buffers :", buffers);

    auto const buffer = std::make_unique<uint8_t[]>(buffer_size);
    BufferQueue bufferQueue{buffer.get(), buffer_size};

    RNG rng{seed};
    test_buffer_queue(bufferQueue, rng, buffers);
}

void
test_buffer_queue(BufferQueue &buffer_queue, RNG &rng, size_t functions) noexcept {
    StartFlag start_flag;

    std::jthread reader{[&, functions]() mutable {
        start_flag.wait();
        size_t total_bytes{0};
        size_t hash{0};
        {
            Timer timer{"reader"};
            while (functions) {
                if (buffer_queue.reserve()) {
                    buffer_queue.consume([&hash, &total_bytes](auto buffer, auto size) {
                        total_bytes += size;
                        std::span const data{reinterpret_cast<char *>(buffer), size};
                        boost::hash_range(hash, data.begin(), data.end());
                    });

                    /*{
                        auto const block = buffer_queue.consume();
                        auto const[buffer, size] = block.get();
                        total_bytes += size;
                        std::span const data{std::bit_cast<char *>(buffer), size};
                        boost::hash_range(hash, data.begin(), data.end());
                    }*/

                    --functions;
                } else {
                    std::this_thread::yield();
                }
            }
        }
        println("reader hash :", hash, ", bytes read :", total_bytes, '\n');
    }};

    std::jthread writer{[&, functions] {
        start_flag.wait();
        auto func = functions;
        size_t total_bytes{0};
        constexpr uint32_t min_buffer_size{3000};
        while (func) {

            /*if (auto const[buffer, avl_size] = buffer_queue.allocate(min_buffer_size); buffer) {
                auto const fill_bytes = rng.getRand<uint32_t>(10, min_buffer_size);
                total_bytes += fill_bytes;
                std::span const data{reinterpret_cast<char *>(buffer), fill_bytes};
                rng.fillRand<char>(std::numeric_limits<char>::min(), std::numeric_limits<char>::max(), data.begin(),
                                   data.end());
                buffer_queue.release(fill_bytes);
                --func;
            }*/


//            if (auto space = buffer_queue.available_space(); space >= min_buffer_size)
            buffer_queue.allocate_and_release(min_buffer_size,
                                              [&, min_buffer_size](auto buffer, auto avl_size) mutable {
                                                  auto const fill_bytes{
                                                          rng.getRand<uint32_t>(10, min_buffer_size)};
                                                  total_bytes += fill_bytes;
                                                  std::span const data{reinterpret_cast<char *>(buffer),
                                                                       fill_bytes};
                                                  rng.fillRand<char>(std::numeric_limits<char>::min(),
                                                                     std::numeric_limits<char>::max(),
                                                                     data.begin(),
                                                                     data.end());
                                                  --func;
                                                  return fill_bytes;
                                              });
        }
        println("bytes written :", total_bytes, '\n');
    }};

    start_flag.start();
}