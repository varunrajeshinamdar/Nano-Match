#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace nanomatch {

/// Whether a book asks the kernel to back its arrays with 2 MiB pages.
///
/// Larger pages mean fewer TLB entries for the same memory, so fewer page table walks. The choice
/// is a template parameter of the book so that its effect can be measured rather than assumed.
enum class PagePolicy : std::uint8_t {
    kDefault,
    kHuge,
};

/// Array of fixed size, mapped once at construction and never resized.
///
/// Mapping and constructing every element here means every page used later has already been faulted
/// in, so the matching path never pays for a page fault or an allocator. The size is decided at
/// startup and doesn't change, and no element is ever destroyed individually, so elements must be
/// trivially destructible.
///
/// Throws std::bad_alloc if the memory can't be mapped.
template <typename T>
class FixedArray {
    static_assert(std::is_trivially_destructible_v<T>);

  public:
    FixedArray(std::size_t count, PagePolicy pages) : count_{count} {
        static_assert(alignof(T) <= kHugePageSize);
        const std::size_t bytes = round_up(count * sizeof(T));

        // A huge page can only back a 2 MiB-aligned range, and mmap only guarantees page alignment,
        // so this maps one extra huge page and starts at the first aligned address within it.
        mapped_size_ = bytes + kHugePageSize;
        mapped_ =
            mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped_ == MAP_FAILED) {
            throw std::bad_alloc{};
        }
        const auto address = reinterpret_cast<std::uintptr_t>(mapped_);
        const std::size_t padding = round_up(address) - address;
        data_ = reinterpret_cast<T*>(static_cast<std::byte*>(mapped_) + padding);

        // This is only advice, and failure just means normal pages, so the result is ignored. It
        // has to come before the first touch, because that's when the kernel chooses the page size.
        static_cast<void>(
            madvise(data_, bytes, pages == PagePolicy::kHuge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE));
        std::uninitialized_value_construct_n(data_, count);
    }

    FixedArray(const FixedArray&) = delete;
    FixedArray& operator=(const FixedArray&) = delete;
    FixedArray(FixedArray&&) = delete;
    FixedArray& operator=(FixedArray&&) = delete;
    ~FixedArray() { munmap(mapped_, mapped_size_); }

    [[nodiscard]] T& operator[](std::size_t index) noexcept { return data_[index]; }
    [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return data_[index]; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

  private:
    static constexpr std::size_t kHugePageSize = std::size_t{2} * 1024 * 1024;

    [[nodiscard]] static constexpr std::size_t round_up(std::size_t bytes) noexcept {
        return (bytes + kHugePageSize - 1) & ~(kHugePageSize - 1);
    }

    T* data_{nullptr};
    std::size_t count_;
    void* mapped_{nullptr};
    std::size_t mapped_size_{0};
};

}  // namespace nanomatch
