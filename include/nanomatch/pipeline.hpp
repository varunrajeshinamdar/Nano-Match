#pragma once

#include <immintrin.h>

#include <cstddef>
#include <span>
#include <thread>

#include "nanomatch/book.hpp"
#include "nanomatch/protocol.hpp"

namespace nanomatch {

/// Sends every command through the ring, spinning whenever it's full. `_mm_pause` tells the CPU
/// this is a wait loop, which saves power and avoids a costly pipeline flush on leaving the loop.
/// Spinning rather than sleeping keeps latency low at the cost of a fully occupied core.
template <typename Ring>
void publish_commands(std::span<const Command> commands, Ring& ring) noexcept {
    for (const Command& command : commands) {
        while (!ring.try_push(command)) {
            _mm_pause();
        }
    }
}

/// Applies `count` commands from the ring to the book, in the order they were sent.
template <typename Ring, typename Book, EventSink Sink>
void consume_commands(std::size_t count, Ring& ring, Book& book, Sink& sink) noexcept {
    Command command;
    for (std::size_t applied = 0; applied < count; ++applied) {
        while (!ring.try_pop(command)) {
            _mm_pause();
        }
        book.apply(command, sink);
    }
}

/// Runs commands through a gateway thread, the ring, and the book on the calling thread. The ring
/// keeps commands in order, so a book's output is the same as if the commands were applied on one
/// thread.
template <typename Ring, typename Book, EventSink Sink>
void run_pipeline(std::span<const Command> commands, Ring& ring, Book& book, Sink& sink) {
    std::thread gateway{[&ring, commands] { publish_commands(commands, ring); }};
    consume_commands(commands.size(), ring, book, sink);
    gateway.join();
}

}  // namespace nanomatch
