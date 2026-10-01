// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"

namespace tobsv {

// Durable append-only record log.
//
// Layout
//   header  64 bytes, written once when the log is created:
//             [0,8)   magic "TOBSVLOG"
//             [8,12)  u32 format version
//             [12,16) u32 header size
//             [16,24) u64 writer epoch recorded at creation, for diagnosis only
//             [24,32) i64 creation timestamp in Unix nanoseconds
//             [32,36) u32 flags (reserved, zero)
//             [36,40) u32 reserved
//             [40,48) u64 reserved
//             [48,52) u32 CRC-32C over bytes [0,48)
//             [52,64) reserved
//   record  frame = u32 payload length, u32 CRC-32C of the payload, u64 sequence, payload bytes
//
// The commit point of a record is the successful completion of a single write of the whole frame
// followed by a durable device flush. Nothing before that point is a committed record, and a frame
// that is incomplete or whose checksum does not match is never treated as one.
//
// Recovery policy
//   * a header that is incomplete, or whose checksum or magic is wrong, is refused as corruption
//     unless the file is shorter than a header, in which case no record could ever have been
//     committed and the log is re-initialised with the fact reported;
//   * a frame that runs past the end of the file is a torn tail: everything from the start of that
//     frame is discarded and the file is truncated to the last committed offset on the next open
//     for writing;
//   * a frame that fits but fails its checksum, with more bytes after it, is interior corruption:
//     the load is refused outright, because silently discarding the middle of a log would hide
//     exactly the damage an operator needs to see;
//   * a frame that fits, fails its checksum, and ends exactly at the end of the file is treated as
//     a torn tail, because a partial device write can leave a complete length with a partial body.
class ThermalLog {
 public:
  ThermalLog() = default;
  ~ThermalLog();

  ThermalLog(const ThermalLog&) = delete;
  ThermalLog& operator=(const ThermalLog&) = delete;
  ThermalLog(ThermalLog&& other) noexcept;
  ThermalLog& operator=(ThermalLog&& other) noexcept;

  enum class OpenMode : std::uint8_t {
    kCreateNew = 0,   // fails when the file already holds a log
    kOpenExisting,    // fails when the file is absent
    kOpenOrCreate,    // opens a valid log or creates one
  };

  struct LoadResult {
    std::vector<std::string> payloads;
    std::uint64_t next_sequence = 1;
    std::uint64_t file_bytes = 0;
    std::uint64_t committed_bytes = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::uint64_t writer_epoch = 0;
    bool reinitialised_short_header = false;
    bool created = false;
  };

  // Reads and validates a log without opening it for writing. This is the only way to inspect a
  // log, and it never mutates the file.
  static Result<LoadResult> inspect(const std::filesystem::path& path, const Limits& limits);

  // Opens a log for appending. A damaged tail is truncated to the last committed offset before the
  // first append; interior corruption refuses the open.
  Status open(const std::filesystem::path& path, OpenMode mode, std::uint64_t writer_epoch,
              const Limits& limits);

  Status close();
  bool is_open() const noexcept { return file_ != nullptr; }

  // True when a write or flush failed part-way. The log then refuses further appends until it is
  // closed and reopened, because the tail is no longer known to end at a committed offset.
  bool poisoned() const noexcept { return poisoned_; }

  // Appends one frame. When durable is true the call does not return until the device has flushed,
  // which is the commit point.
  Result<std::uint64_t> append(std::string_view payload, bool durable);

  Status flush();

  // What the open recovered. Callers use this instead of inspecting the file again: the writer
  // already holds the only supported view of its own committed prefix.
  const LoadResult& load_result() const noexcept { return load_; }

  std::uint64_t next_sequence() const noexcept { return next_sequence_; }
  std::uint64_t committed_bytes() const noexcept { return committed_bytes_; }
  std::uint64_t discarded_tail_bytes() const noexcept { return discarded_tail_bytes_; }
  const std::filesystem::path& path() const noexcept { return path_; }

  static constexpr std::size_t kHeaderSize = 64;
  static constexpr std::size_t kFrameOverhead = 16;
  static constexpr std::string_view kMagic = "TOBSVLOG";

 private:
  // Rolls the file back to the last committed offset after a failed write or flush.
  void roll_back_tail();

  void* file_ = nullptr;
  std::filesystem::path path_;
  Limits limits_;
  LoadResult load_;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t committed_bytes_ = 0;
  std::uint64_t discarded_tail_bytes_ = 0;
  bool poisoned_ = false;
};

}  // namespace tobsv
