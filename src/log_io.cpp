#include "nanomatch/log_io.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <vector>

namespace nanomatch {
namespace {

[[nodiscard]] std::string describe(const std::filesystem::path& path, const char* problem) {
    return path.string() + ": " + problem;
}

[[nodiscard]] char* char_data(std::span<std::byte> bytes) noexcept {
    return reinterpret_cast<char*>(bytes.data());
}

[[nodiscard]] const char* char_data(std::span<const std::byte> bytes) noexcept {
    return reinterpret_cast<const char*>(bytes.data());
}

[[nodiscard]] constexpr LogHeader make_header(const std::array<char, 8>& magic,
                                              std::uint16_t record_size) noexcept {
    return LogHeader{.magic = magic, .version = kLogVersion, .record_size = record_size};
}

/// Reads the header and all records, leaving content checks on the records to the caller.
template <typename Record>
[[nodiscard]] std::vector<Record> read_records(const std::filesystem::path& path,
                                               const std::array<char, 8>& magic) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        throw LogError{describe(path, "cannot open for reading")};
    }
    const auto file_size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);

    if (file_size < sizeof(LogHeader)) {
        throw LogError{describe(path, "file is smaller than the log header")};
    }
    LogHeader header;
    file.read(char_data(std::as_writable_bytes(std::span{&header, 1})), sizeof(header));
    if (header.magic != magic) {
        throw LogError{describe(path, "wrong magic value")};
    }
    if (header.version != kLogVersion) {
        throw LogError{describe(path, "unsupported version")};
    }
    if (header.record_size != sizeof(Record)) {
        throw LogError{describe(path, "wrong record size")};
    }
    if (header.reserved != 0) {
        throw LogError{describe(path, "reserved header bytes are not zero")};
    }

    const std::size_t payload_size = file_size - sizeof(LogHeader);
    if (payload_size % sizeof(Record) != 0) {
        throw LogError{describe(path, "file does not hold a whole number of records")};
    }
    std::vector<Record> records(payload_size / sizeof(Record));
    file.read(char_data(std::as_writable_bytes(std::span{records})),
              static_cast<std::streamsize>(payload_size));
    if (!file) {
        throw LogError{describe(path, "read failed")};
    }
    return records;
}

}  // namespace

std::vector<Command> read_command_log(const std::filesystem::path& path) {
    std::vector<Command> commands = read_records<Command>(path, kCommandLogMagic);
    for (std::size_t i = 1; i < commands.size(); ++i) {
        if (commands[i].seq <= commands[i - 1].seq) {
            throw LogError{describe(path, "seq is not strictly increasing")};
        }
    }
    return commands;
}

std::vector<Event> read_event_log(const std::filesystem::path& path) {
    return read_records<Event>(path, kEventLogMagic);
}

void write_command_log(const std::filesystem::path& path, std::span<const Command> commands) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        throw LogError{describe(path, "cannot open for writing")};
    }
    const LogHeader header = make_header(kCommandLogMagic, sizeof(Command));
    file.write(char_data(std::as_bytes(std::span{&header, 1})), sizeof(header));
    const std::span<const std::byte> records = std::as_bytes(commands);
    file.write(char_data(records), static_cast<std::streamsize>(records.size()));
    file.flush();
    if (!file) {
        throw LogError{describe(path, "write failed")};
    }
}

EventLogSink::EventLogSink() {
    const LogHeader header = make_header(kEventLogMagic, sizeof(Event));
    write_bytes(std::as_bytes(std::span{&header, 1}));
}

EventLogSink::EventLogSink(const std::filesystem::path& path)
    : file_{path, std::ios::binary | std::ios::trunc} {
    if (!file_) {
        throw LogError{describe(path, "cannot open for writing")};
    }
    const LogHeader header = make_header(kEventLogMagic, sizeof(Event));
    write_bytes(std::as_bytes(std::span{&header, 1}));
}

void EventLogSink::on_event(const Event& event) noexcept {
    write_bytes(std::as_bytes(std::span{&event, 1}));
    ++event_count_;
}

void EventLogSink::write_bytes(std::span<const std::byte> bytes) noexcept {
    hash_.update(bytes);
    if (file_.is_open()) {
        file_.write(char_data(bytes), static_cast<std::streamsize>(bytes.size()));
    }
}

void EventLogSink::finish() {
    if (file_.is_open()) {
        file_.flush();
        if (!file_) {
            throw LogError{"event log write failed"};
        }
    }
}

}  // namespace nanomatch
