#pragma once

#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "baseline/baseline_book.hpp"
#include "baseline/pool_allocator.hpp"
#include "nanomatch/order_book.hpp"

namespace nanomatch::tools {

/// Calls `visitor.template operator()<Book>()` with the book type a name refers to. Keeping the
/// names in one place means every tool accepts the same set. Throws std::invalid_argument for an
/// unknown name.
template <typename Visitor>
decltype(auto) with_book(std::string_view name, Visitor&& visitor) {
    if (name == "baseline") {
        return std::forward<Visitor>(visitor).template operator()<baseline::BaselineBook<>>();
    }
    if (name == "baseline_pool") {
        return std::forward<Visitor>(visitor)
            .template operator()<baseline::BaselineBook<baseline::PoolAllocator>>();
    }
    if (name == "fast") {
        return std::forward<Visitor>(visitor).template operator()<OrderBook<PagePolicy::kHuge>>();
    }
    if (name == "fast_4k") {
        return std::forward<Visitor>(visitor)
            .template operator()<OrderBook<PagePolicy::kDefault>>();
    }
    throw std::invalid_argument{std::format("unknown book '{}'", name)};
}

}  // namespace nanomatch::tools
