#include "nanomatch/protocol.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

namespace nanomatch {
namespace {

// Layout is asserted in the header. These tests pin the values that end up in log files, because
// renumbering an enum would silently change the format without changing any layout.

TEST(ProtocolTest, CommandTypeValuesMatchSpecification) {
    EXPECT_EQ(static_cast<int>(CommandType::kLimit), 1);
    EXPECT_EQ(static_cast<int>(CommandType::kMarket), 2);
    EXPECT_EQ(static_cast<int>(CommandType::kCancel), 3);
    EXPECT_EQ(static_cast<int>(CommandType::kModify), 4);
}

TEST(ProtocolTest, SideValuesMatchSpecification) {
    EXPECT_EQ(static_cast<int>(Side::kNone), 0);
    EXPECT_EQ(static_cast<int>(Side::kBuy), 1);
    EXPECT_EQ(static_cast<int>(Side::kSell), 2);
}

TEST(ProtocolTest, EventTypeValuesMatchSpecification) {
    EXPECT_EQ(static_cast<int>(EventType::kAccepted), 1);
    EXPECT_EQ(static_cast<int>(EventType::kRejected), 2);
    EXPECT_EQ(static_cast<int>(EventType::kTrade), 3);
    EXPECT_EQ(static_cast<int>(EventType::kRested), 4);
    EXPECT_EQ(static_cast<int>(EventType::kCanceled), 5);
    EXPECT_EQ(static_cast<int>(EventType::kModified), 6);
}

TEST(ProtocolTest, ReasonValuesMatchSpecification) {
    EXPECT_EQ(static_cast<int>(Reason::kNone), 0);
    EXPECT_EQ(static_cast<int>(Reason::kMalformed), 1);
    EXPECT_EQ(static_cast<int>(Reason::kDuplicateId), 2);
    EXPECT_EQ(static_cast<int>(Reason::kUnknownId), 3);
    EXPECT_EQ(static_cast<int>(Reason::kInvalidPrice), 4);
    EXPECT_EQ(static_cast<int>(Reason::kInvalidQuantity), 5);
    EXPECT_EQ(static_cast<int>(Reason::kUserCancel), 6);
    EXPECT_EQ(static_cast<int>(Reason::kMarketRemainder), 7);
    EXPECT_EQ(static_cast<int>(Reason::kBookFull), 8);
}

TEST(ProtocolTest, LogMagicMatchesSpecification) {
    EXPECT_EQ(std::memcmp(kCommandLogMagic.data(), "NMCMD\0\0\0", 8), 0);
    EXPECT_EQ(std::memcmp(kEventLogMagic.data(), "NMEVT\0\0\0", 8), 0);
    EXPECT_EQ(kLogVersion, 1);
}

TEST(ProtocolTest, DefaultCommandIsAllZeroBytes) {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(Command)>>(Command{});
    for (const std::byte byte : bytes) {
        EXPECT_EQ(byte, std::byte{0});
    }
}

TEST(ProtocolTest, DefaultEventIsAllZeroBytes) {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(Event)>>(Event{});
    for (const std::byte byte : bytes) {
        EXPECT_EQ(byte, std::byte{0});
    }
}

TEST(ProtocolTest, CommandEncodesToSpecifiedBytes) {
    const Command command{.seq = Sequence{0x0102},
                          .order_id = OrderId{0x0304},
                          .price = Price{0x0506},
                          .quantity = Quantity{0x0708},
                          .type = CommandType::kModify,
                          .side = Side::kSell,
                          .reserved = 0};
    const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(Command)>>(command);

    // Each multi-byte field stores its least significant byte first.
    EXPECT_EQ(bytes[0], 0x02);
    EXPECT_EQ(bytes[1], 0x01);
    EXPECT_EQ(bytes[8], 0x04);
    EXPECT_EQ(bytes[9], 0x03);
    EXPECT_EQ(bytes[16], 0x06);
    EXPECT_EQ(bytes[17], 0x05);
    EXPECT_EQ(bytes[24], 0x08);
    EXPECT_EQ(bytes[25], 0x07);
    EXPECT_EQ(bytes[28], 4);
    EXPECT_EQ(bytes[29], 2);
}

TEST(ProtocolTest, UnknownEnumValueSurvivesRoundTrip) {
    // Validation depends on being able to see a bad type byte from a log rather than losing it.
    std::array<std::uint8_t, sizeof(Command)> bytes{};
    bytes[28] = 0xFF;
    const auto command = std::bit_cast<Command>(bytes);
    EXPECT_EQ(static_cast<std::uint8_t>(command.type), 0xFF);
}

}  // namespace
}  // namespace nanomatch
