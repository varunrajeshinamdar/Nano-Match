#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace nanomatch::baseline {

/// Standard-library allocator that serves single objects from per-type free lists.
///
/// Running the baseline's containers on this separates what comes from avoiding the general-purpose
/// allocator from what comes from different data structures. It has no bearing on the optimized
/// book, which doesn't use standard containers at all.
///
/// Requests for several objects at once, such as a hash map's bucket array, go to the normal
/// allocator, because they vary in size and are rare. Memory is grown in chunks when a free list
/// runs out, and isn't returned until exit. State is shared per type and isn't thread-safe.
template <typename T>
class PoolAllocator {
  public:
    using value_type = T;

    PoolAllocator() noexcept = default;

    // NOLINTNEXTLINE(google-explicit-constructor): the standard requires this to be implicit.
    template <typename U>
    PoolAllocator(const PoolAllocator<U>& /*other*/) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count != 1) {
            return std::allocator<T>{}.allocate(count);
        }
        if (free_head() == nullptr) {
            grow();
        }
        Block* const block = free_head();
        free_head() = block->next;
        return reinterpret_cast<T*>(block);
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        if (count != 1) {
            std::allocator<T>{}.deallocate(pointer, count);
            return;
        }
        auto* const block = reinterpret_cast<Block*>(pointer);
        block->next = free_head();
        free_head() = block;
    }

    friend bool operator==(const PoolAllocator&, const PoolAllocator&) noexcept { return true; }

  private:
    union Block {
        Block* next{nullptr};
        // NOLINTNEXTLINE(bugprone-sizeof-expression): T is sometimes a pointer, which is fine here.
        alignas(T) std::array<std::byte, sizeof(T)> storage;
    };

    static constexpr std::size_t kBlocksPerChunk = std::size_t{16} * 1024;

    [[nodiscard]] static Block*& free_head() noexcept {
        static Block* head = nullptr;
        return head;
    }

    static void grow() {
        static std::vector<std::vector<Block>> chunks;
        chunks.emplace_back(kBlocksPerChunk);
        std::vector<Block>& blocks = chunks.back();
        for (std::size_t i = 0; i < kBlocksPerChunk; ++i) {
            blocks[i].next = i + 1 < kBlocksPerChunk ? &blocks[i + 1] : free_head();
        }
        free_head() = blocks.data();
    }
};

}  // namespace nanomatch::baseline
