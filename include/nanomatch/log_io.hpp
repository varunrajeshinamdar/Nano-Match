#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

#include "nanomatch/fnv1a.hpp"
#include "nanomatch/protocol.hpp"

/// Reading and writing the log formats in section 7 of docs/SPEC.md. Everything here runs outside
/// the matching path, so it uses streams and exceptions freely.

namespace nanomatch {

/// A log file could not be read or written, or its contents are corrupt.
class LogError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Reads a whole command log. Throws LogError if the header is invalid, the file doesn't hold a
/// whole number of records, or `seq` is not strictly increasing.
[[nodiscard]] std::vector<Command> read_command_log(const std::filesystem::path& path);

/// Reads a whole event log. Throws LogError if the header is invalid or the file doesn't hold a
/// whole number of records.
[[nodiscard]] std::vector<Event> read_event_log(const std::filesystem::path& path);

/// Writes a command log. Throws LogError on failure.
void write_command_log(const std::filesystem::path& path, std::span<const Command> commands);

/// Event sink that hashes the event log it produces and optionally writes it to a file. The hash
/// covers the same bytes as the file, header included, so it can be checked against the file
/// itself.
class EventLogSink {
  public:
    /// Hashes without writing a file.
    EventLogSink();

    /// Hashes and writes to `path`. Throws LogError if the file can't be opened.
    explicit EventLogSink(const std::filesystem::path& path);

    void on_event(const Event& event) noexcept;

    /// Flushes the file. Throws LogError if any earlier write failed. Sinks can't report failures
    /// from `on_event`, so this call is what makes a written log trustworthy.
    void finish();

    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_.value(); }
    [[nodiscard]] std::uint64_t event_count() const noexcept { return event_count_; }

  private:
    void write_bytes(std::span<const std::byte> bytes) noexcept;

    std::ofstream file_;
    Fnv1a64 hash_;
    std::uint64_t event_count_{0};
};

}  // namespace nanomatch
