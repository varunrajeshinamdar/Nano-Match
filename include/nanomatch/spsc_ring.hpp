#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace nanomatch {

/// How a ring lays out its shared state. The default is the intended configuration. The others
/// exist so that a benchmark can show what each choice contributes, rather than assuming it.
struct SpscRingLayout {
    /// Alignment of each index, in bytes. 64 gives each its own cache line on x86.
    std::size_t index_alignment{64};
    /// Whether each side keeps a private copy of the other side's index.
    bool cache_indices{true};
};

/// Fixed-capacity ring buffer for one producer thread and one consumer thread.
///
/// Neither side ever takes a lock or retries a failed atomic operation, so each call finishes in a
/// bounded number of its own steps whatever the other thread is doing. That makes both operations
/// wait-free, a stronger property than lock-free. A full or empty ring returns false instead of
/// waiting, and the caller decides how to wait.
///
/// The two indices increase forever and are masked only to find a slot. A 64-bit counter can't wrap
/// in practice, and this avoids the usual wasted slot, because `tail - head` is exact for both full
/// and empty.
///
/// Performance comes from limiting cache-line traffic between the two cores:
/// - Each index lives on its own cache line. Otherwise, a write to one would invalidate the other
/// core's
///   copy of a value it wanted to read. That's false sharing, because the threads share no actual
///   data.
/// - Each side keeps a private copy of the other's index and only rereads the shared one when the
/// copy
///   says the ring looks full or empty. Most operations then involve no read of a contended line.
template <typename T, std::size_t Capacity, SpscRingLayout Layout = SpscRingLayout{}>
class SpscRing {
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(Layout.index_alignment >= alignof(std::atomic<std::uint64_t>));

  public:
    SpscRing() noexcept {
        // Offsets are only available once the class is complete, which is why this check is here.
        static_assert(offsetof(SpscRing, cached_head_) - offsetof(SpscRing, tail_) >=
                      Layout.index_alignment);
        static_assert(offsetof(SpscRing, head_) - offsetof(SpscRing, cached_head_) >=
                      Layout.index_alignment);
        static_assert(offsetof(SpscRing, cached_tail_) - offsetof(SpscRing, head_) >=
                      Layout.index_alignment);
        static_assert(offsetof(SpscRing, slots_) - offsetof(SpscRing, cached_tail_) >=
                      Layout.index_alignment);
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    ~SpscRing() = default;

    /// Producer thread only. Returns false if the ring is full.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        // Relaxed: only this thread writes tail_, so it already sees the latest value.
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (Layout.cache_indices) {
            if (tail - cached_head_ == Capacity) {
                // Acquire pairs with the release in try_pop: once a slot is seen as free, the
                // consumer's read of that slot has finished, so overwriting it can't race.
                cached_head_ = head_.load(std::memory_order_acquire);
                if (tail - cached_head_ == Capacity) {
                    return false;
                }
            }
        } else if (tail - head_.load(std::memory_order_acquire) == Capacity) {
            // Acquire for the same reason as above.
            return false;
        }
        slots_[tail & kMask] = item;
        // Release pairs with the acquire in try_pop: a consumer that sees this index also sees the
        // item written above.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer thread only. Returns false if the ring is empty, leaving `item` untouched.
    [[nodiscard]] bool try_pop(T& item) noexcept {
        // Relaxed: only this thread writes head_, so it already sees the latest value.
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        if (Layout.cache_indices) {
            if (head == cached_tail_) {
                // Acquire pairs with the release in try_push: seeing a new tail guarantees the
                // items before it are visible.
                cached_tail_ = tail_.load(std::memory_order_acquire);
                if (head == cached_tail_) {
                    return false;
                }
            }
        } else if (head == tail_.load(std::memory_order_acquire)) {
            // Acquire for the same reason as above.
            return false;
        }
        item = slots_[head & kMask];
        // Release pairs with the acquire in try_push: the producer can only reuse this slot after
        // seeing this index, by which time the read above has finished.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    alignas(Layout.index_alignment) std::atomic<std::uint64_t> tail_{0};
    alignas(Layout.index_alignment) std::uint64_t cached_head_{0};
    alignas(Layout.index_alignment) std::atomic<std::uint64_t> head_{0};
    alignas(Layout.index_alignment) std::uint64_t cached_tail_{0};
    alignas(Layout.index_alignment) std::array<T, Capacity> slots_{};
};

}  // namespace nanomatch
