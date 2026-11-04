#include "nanomatch/log_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "nanomatch/fnv1a.hpp"
#include "test_support.hpp"

namespace nanomatch {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::uint64_t hash_of(std::string_view text) {
    Fnv1a64 hash;
    hash.update(std::as_bytes(std::span{text}));
    return hash.value();
}

TEST(Fnv1a64Test, MatchesPublishedTestVectors) {
    EXPECT_EQ(hash_of(""), 0xcbf29ce484222325ULL);
    EXPECT_EQ(hash_of("a"), 0xaf63dc4c8601ec8cULL);
    EXPECT_EQ(hash_of("foobar"), 0x85944171f73967e8ULL);
}

TEST(Fnv1a64Test, IncrementalUpdatesMatchSingleUpdate) {
    Fnv1a64 hash;
    const std::string_view foo = "foo";
    const std::string_view bar = "bar";
    hash.update(std::as_bytes(std::span{foo}));
    hash.update(std::as_bytes(std::span{bar}));
    EXPECT_EQ(hash.value(), hash_of("foobar"));
}

class LogIoTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const auto* const info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string{"nanomatch_"} + info->name());
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] fs::path path(const std::string& name) const { return dir_ / name; }

    [[nodiscard]] static std::vector<Command> sample_commands() {
        std::vector<Command> commands{test::make_limit(10, Side::kSell, 100, 5),
                                      test::make_limit(11, Side::kBuy, 101, 8),
                                      test::make_cancel(11)};
        std::uint64_t seq = 0;
        for (Command& command : commands) {
            command.seq = Sequence{++seq};
        }
        return commands;
    }

    [[nodiscard]] static std::vector<char> bytes_of(const fs::path& file) {
        std::ifstream in{file, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    static void write_bytes(const fs::path& file, const std::vector<char>& bytes) {
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    /// Writes a valid command log, corrupts one byte, and returns the path.
    [[nodiscard]] fs::path corrupted_log(std::size_t offset, char value) const {
        const fs::path file = path("corrupt.nmc");
        write_command_log(file, sample_commands());
        std::vector<char> bytes = bytes_of(file);
        bytes.at(offset) = value;
        write_bytes(file, bytes);
        return file;
    }

  private:
    fs::path dir_;
};

TEST_F(LogIoTest, CommandLogRoundTrips) {
    const fs::path file = path("commands.nmc");
    const std::vector<Command> commands = sample_commands();
    write_command_log(file, commands);
    const std::vector<Command> read_back = read_command_log(file);
    ASSERT_EQ(read_back.size(), commands.size());
    EXPECT_EQ(std::memcmp(read_back.data(), commands.data(), commands.size() * sizeof(Command)), 0);
}

TEST_F(LogIoTest, CommandLogHasSpecifiedSize) {
    const fs::path file = path("commands.nmc");
    write_command_log(file, sample_commands());
    EXPECT_EQ(fs::file_size(file), 16U + 3U * 32U);
}

TEST_F(LogIoTest, EmptyCommandLogIsValid) {
    const fs::path file = path("empty.nmc");
    write_command_log(file, {});
    EXPECT_TRUE(read_command_log(file).empty());
}

TEST_F(LogIoTest, MissingFileIsAnError) {
    EXPECT_THROW((void)read_command_log(path("missing.nmc")), LogError);
}

TEST_F(LogIoTest, FileSmallerThanHeaderIsCorrupt) {
    const fs::path file = path("short.nmc");
    write_bytes(file, std::vector<char>(15, 'x'));
    EXPECT_THROW((void)read_command_log(file), LogError);
}

TEST_F(LogIoTest, WrongMagicIsCorrupt) {
    EXPECT_THROW((void)read_command_log(corrupted_log(0, 'X')), LogError);
}

TEST_F(LogIoTest, EventLogIsNotACommandLog) {
    const fs::path file = path("events.nme");
    {
        EventLogSink sink{file};
        sink.finish();
    }
    EXPECT_THROW((void)read_command_log(file), LogError);
}

TEST_F(LogIoTest, WrongVersionIsCorrupt) {
    EXPECT_THROW((void)read_command_log(corrupted_log(8, 2)), LogError);
}

TEST_F(LogIoTest, WrongRecordSizeIsCorrupt) {
    EXPECT_THROW((void)read_command_log(corrupted_log(10, 31)), LogError);
}

TEST_F(LogIoTest, NonZeroReservedHeaderIsCorrupt) {
    EXPECT_THROW((void)read_command_log(corrupted_log(12, 1)), LogError);
}

TEST_F(LogIoTest, PartialRecordIsCorrupt) {
    const fs::path file = path("partial.nmc");
    write_command_log(file, sample_commands());
    std::vector<char> bytes = bytes_of(file);
    bytes.pop_back();
    write_bytes(file, bytes);
    EXPECT_THROW((void)read_command_log(file), LogError);
}

TEST_F(LogIoTest, RepeatedSeqIsCorrupt) {
    std::vector<Command> commands = sample_commands();
    commands[1].seq = commands[0].seq;
    const fs::path file = path("repeated.nmc");
    write_command_log(file, commands);
    EXPECT_THROW((void)read_command_log(file), LogError);
}

TEST_F(LogIoTest, DecreasingSeqIsCorrupt) {
    std::vector<Command> commands = sample_commands();
    commands[2].seq = Sequence{1};
    const fs::path file = path("decreasing.nmc");
    write_command_log(file, commands);
    EXPECT_THROW((void)read_command_log(file), LogError);
}

TEST_F(LogIoTest, SinkHashCoversTheWholeFile) {
    const fs::path file = path("events.nme");
    EventLogSink sink{file};
    sink.on_event(Event{.event_seq = Sequence{1}, .type = EventType::kAccepted});
    sink.on_event(Event{.event_seq = Sequence{2}, .type = EventType::kRested});
    sink.finish();

    const std::vector<char> bytes = bytes_of(file);
    Fnv1a64 expected;
    expected.update(std::as_bytes(std::span{bytes}));
    EXPECT_EQ(sink.hash(), expected.value());
    EXPECT_EQ(sink.event_count(), 2U);
    EXPECT_EQ(bytes.size(), 16U + 2U * 56U);
}

TEST_F(LogIoTest, HashIsTheSameWithAndWithoutAFile) {
    const Event event{.event_seq = Sequence{1}, .type = EventType::kAccepted};
    EventLogSink to_file{path("events.nme")};
    EventLogSink hash_only;
    to_file.on_event(event);
    hash_only.on_event(event);
    to_file.finish();
    hash_only.finish();
    EXPECT_EQ(to_file.hash(), hash_only.hash());
}

TEST_F(LogIoTest, EventLogRoundTrips) {
    const fs::path file = path("events.nme");
    const Event event{.event_seq = Sequence{1}, .order_id = OrderId{9}, .type = EventType::kRested};
    {
        EventLogSink sink{file};
        sink.on_event(event);
        sink.finish();
    }
    const std::vector<Event> read_back = read_event_log(file);
    ASSERT_EQ(read_back.size(), 1U);
    EXPECT_EQ(std::memcmp(read_back.data(), &event, sizeof(Event)), 0);
}

TEST_F(LogIoTest, UnwritableEventLogIsAnError) {
    EXPECT_THROW(EventLogSink{path("no_such_directory") / "events.nme"}, LogError);
}

TEST_F(LogIoTest, UnwritableCommandLogIsAnError) {
    EXPECT_THROW(write_command_log(path("no_such_directory") / "commands.nmc", sample_commands()),
                 LogError);
}

// Every write to /dev/full fails with ENOSPC, which simulates a full disk.

TEST_F(LogIoTest, FullDiskIsReportedWhenWritingCommands) {
    EXPECT_THROW(write_command_log("/dev/full", sample_commands()), LogError);
}

TEST_F(LogIoTest, FullDiskIsReportedWhenFinishingEventLog) {
    EventLogSink sink{fs::path{"/dev/full"}};
    sink.on_event(Event{.event_seq = Sequence{1}, .type = EventType::kAccepted});
    EXPECT_THROW(sink.finish(), LogError);
}

}  // namespace
}  // namespace nanomatch
