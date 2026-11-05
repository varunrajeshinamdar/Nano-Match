// Proves that the optimized book never touches the heap while processing commands.
//
// This is its own executable because it replaces the C allocator for the whole process. Everything
// that reaches the heap, including `operator new` and the standard containers, ends up in these
// functions, so nothing can slip past by allocating in a less obvious way. Sanitizers replace the
// allocator too, so this test isn't built in those configurations.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "baseline/baseline_book.hpp"
#include "command_stream.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/order_book.hpp"
#include "workload/generator.hpp"

namespace {

// Written from only the thread under test, and only read after it stops.
bool heap_watch_armed = false;
std::uint64_t heap_calls_seen = 0;

void note_heap_call() noexcept {
    if (heap_watch_armed) {
        ++heap_calls_seen;
    }
}

}  // namespace

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
extern "C" {
void* __libc_malloc(std::size_t size);
void __libc_free(void* pointer);
void* __libc_calloc(std::size_t count, std::size_t size);
void* __libc_realloc(void* pointer, std::size_t size);
void* __libc_memalign(std::size_t alignment, std::size_t size);

void* malloc(std::size_t size) {
    note_heap_call();
    return __libc_malloc(size);
}
void free(void* pointer) {
    note_heap_call();
    __libc_free(pointer);
}
void* calloc(std::size_t count, std::size_t size) {
    note_heap_call();
    return __libc_calloc(count, size);
}
void* realloc(void* pointer, std::size_t size) {
    note_heap_call();
    return __libc_realloc(pointer, size);
}
void* memalign(std::size_t alignment, std::size_t size) {
    note_heap_call();
    return __libc_memalign(alignment, size);
}
void* aligned_alloc(std::size_t alignment, std::size_t size) {
    note_heap_call();
    return __libc_memalign(alignment, size);
}
int posix_memalign(void** result, std::size_t alignment, std::size_t size) {
    note_heap_call();
    *result = __libc_memalign(alignment, size);
    return *result == nullptr ? 12 /* ENOMEM */ : 0;
}
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

namespace nanomatch::test {
namespace {

/// Counts events without allocating, so that any heap call in a test comes from the book.
struct CountingSink {
    std::uint64_t events{0};
    void on_event(const Event& /*event*/) noexcept { ++events; }
};

/// Runs `commands` through a new book, with the watch armed from the very first command. There's no
/// warmup on purpose: allocating only for early orders would still count as a failure.
template <typename Book>
[[nodiscard]] std::uint64_t heap_calls_during(const BookConfig& config,
                                              const std::vector<Command>& commands,
                                              std::uint64_t& events) {
    Book book{config};
    CountingSink sink;
    heap_calls_seen = 0;
    heap_watch_armed = true;
    for (const Command& command : commands) {
        book.apply(command, sink);
    }
    heap_watch_armed = false;
    events = sink.events;
    return heap_calls_seen;
}

[[nodiscard]] std::vector<Command> generate(std::string_view profile_name, std::size_t count) {
    const workload::Profile* const profile = workload::find_profile(profile_name);
    EXPECT_NE(profile, nullptr);
    workload::Generator generator{*profile, 3};
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        commands.push_back(generator.next());
    }
    return commands;
}

constexpr std::size_t kCommands = 1'000'000;

// The first two tests are controls. If the watch didn't work, the zero counts below would mean
// nothing.

TEST(HeapWatchTest, SeesDirectCalls) {
    heap_calls_seen = 0;
    heap_watch_armed = true;
    void* volatile pointer = std::malloc(64);
    std::free(pointer);
    heap_watch_armed = false;
    EXPECT_GE(heap_calls_seen, 2U);
}

TEST(HeapWatchTest, SeesOperatorNewAndContainers) {
    heap_calls_seen = 0;
    heap_watch_armed = true;
    {
        std::vector<int> values(1000);
        values[0] = 1;
        void* volatile escape = values.data();
        static_cast<void>(escape);
    }
    heap_watch_armed = false;
    EXPECT_GE(heap_calls_seen, 2U);
}

TEST(NoAllocationTest, BaselineDoesAllocate) {
    const workload::Profile* const profile = workload::find_profile("realistic");
    ASSERT_NE(profile, nullptr);
    const std::vector<Command> commands = generate("realistic", 100'000);
    std::uint64_t events = 0;
    EXPECT_GT(heap_calls_during<baseline::BaselineBook<>>(workload::book_config(*profile), commands,
                                                          events),
              0U);
}

class NoAllocationOnWorkloadTest : public ::testing::TestWithParam<std::string_view> {};

TEST_P(NoAllocationOnWorkloadTest, OptimizedBookNeverTouchesTheHeap) {
    const workload::Profile* const profile = workload::find_profile(GetParam());
    ASSERT_NE(profile, nullptr);
    const std::vector<Command> commands = generate(GetParam(), kCommands);
    std::uint64_t events = 0;
    EXPECT_EQ(heap_calls_during<OrderBook<>>(workload::book_config(*profile), commands, events),
              0U);
    EXPECT_GT(events, kCommands);
}

INSTANTIATE_TEST_SUITE_P(Profiles, NoAllocationOnWorkloadTest,
                         ::testing::Values("realistic", "add_cancel", "sweep", "realistic_1k",
                                           "realistic_100k"));

TEST(NoAllocationTest, RejectsAndFullBookPathsDoNotAllocate) {
    // A tiny capacity makes the full-book path common, and this stream also includes every reject.
    CommandStream stream{5};
    std::vector<Command> commands;
    commands.reserve(kCommands);
    for (std::size_t i = 0; i < kCommands; ++i) {
        commands.push_back(stream.next());
    }
    const BookConfig config{
        .min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 16};
    std::uint64_t events = 0;
    EXPECT_EQ(heap_calls_during<OrderBook<>>(config, commands, events), 0U);
    EXPECT_GT(events, kCommands / 2);
}

}  // namespace
}  // namespace nanomatch::test
