#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "nanomatch/types.hpp"

/// Binary record definitions for section 7 of docs/SPEC.md.
///
/// Records are written to log files as their in-memory bytes, so the assertions in this header are
/// what keep the code and the specification in agreement: a layout change fails the build.

namespace nanomatch {

// Enums use fixed underlying types, so any byte read from a log is a valid value of the enum. An
// unknown value is therefore well-defined and reaches validation, which rejects it as malformed.

enum class CommandType : std::uint8_t {
    kLimit = 1,
    kMarket = 2,
    kCancel = 3,
    kModify = 4,
};

enum class Side : std::uint8_t {
    kNone = 0,
    kBuy = 1,
    kSell = 2,
};

enum class EventType : std::uint8_t {
    kAccepted = 1,
    kRejected = 2,
    kTrade = 3,
    kRested = 4,
    kCanceled = 5,
    kModified = 6,
};

enum class Reason : std::uint8_t {
    kNone = 0,
    kMalformed = 1,
    kDuplicateId = 2,
    kUnknownId = 3,
    kInvalidPrice = 4,
    kInvalidQuantity = 5,
    kUserCancel = 6,
    kMarketRemainder = 7,
    kBookFull = 8,
};

/// One instruction to the book. Unused fields must be zero; see section 6 of the specification.
struct Command {
    Sequence seq{};
    OrderId order_id{};
    Price price{};
    Quantity quantity{};
    CommandType type{};
    Side side{};
    std::uint16_t reserved{};
};

/// One observable outcome of a command. Field meanings per event type are in section 7.2.
struct Event {
    Sequence event_seq{};
    Sequence command_seq{};
    OrderId order_id{};
    OrderId counter_order_id{};
    Price price{};
    Quantity quantity{};
    Quantity leaves{};
    Quantity counter_leaves{};
    EventType type{};
    Side side{};
    Reason reason{};
    std::uint8_t reserved{};
};

/// Header at the start of every log file.
struct LogHeader {
    std::array<char, 8> magic{};
    std::uint16_t version{};
    std::uint16_t record_size{};
    std::uint32_t reserved{};
};

inline constexpr std::array<char, 8> kCommandLogMagic{'N', 'M', 'C', 'M', 'D', '\0', '\0', '\0'};
inline constexpr std::array<char, 8> kEventLogMagic{'N', 'M', 'E', 'V', 'T', '\0', '\0', '\0'};
inline constexpr std::uint16_t kLogVersion = 1;

// Records are copied to and from files as raw bytes, which assumes the format's byte order.
static_assert(std::endian::native == std::endian::little);

// Unique object representations means there are no padding bytes. Equal records are then equal byte
// for byte, which the determinism guarantee depends on, because logs are compared by hash.
static_assert(std::is_trivially_copyable_v<Command> && std::is_standard_layout_v<Command>);
static_assert(std::has_unique_object_representations_v<Command>);
static_assert(sizeof(Command) == 32);
static_assert(offsetof(Command, seq) == 0);
static_assert(offsetof(Command, order_id) == 8);
static_assert(offsetof(Command, price) == 16);
static_assert(offsetof(Command, quantity) == 24);
static_assert(offsetof(Command, type) == 28);
static_assert(offsetof(Command, side) == 29);
static_assert(offsetof(Command, reserved) == 30);

static_assert(std::is_trivially_copyable_v<Event> && std::is_standard_layout_v<Event>);
static_assert(std::has_unique_object_representations_v<Event>);
static_assert(sizeof(Event) == 56);
static_assert(offsetof(Event, event_seq) == 0);
static_assert(offsetof(Event, command_seq) == 8);
static_assert(offsetof(Event, order_id) == 16);
static_assert(offsetof(Event, counter_order_id) == 24);
static_assert(offsetof(Event, price) == 32);
static_assert(offsetof(Event, quantity) == 40);
static_assert(offsetof(Event, leaves) == 44);
static_assert(offsetof(Event, counter_leaves) == 48);
static_assert(offsetof(Event, type) == 52);
static_assert(offsetof(Event, side) == 53);
static_assert(offsetof(Event, reason) == 54);
static_assert(offsetof(Event, reserved) == 55);

static_assert(std::is_trivially_copyable_v<LogHeader> && std::is_standard_layout_v<LogHeader>);
static_assert(std::has_unique_object_representations_v<LogHeader>);
static_assert(sizeof(LogHeader) == 16);
static_assert(offsetof(LogHeader, magic) == 0);
static_assert(offsetof(LogHeader, version) == 8);
static_assert(offsetof(LogHeader, record_size) == 10);
static_assert(offsetof(LogHeader, reserved) == 12);

}  // namespace nanomatch
