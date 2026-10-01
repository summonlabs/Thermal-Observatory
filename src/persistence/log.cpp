// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/persistence/log.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "tobsv/core/crc32c.hpp"
#include "tobsv/persistence/files.hpp"
#include "tobsv/version.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tobsv {
namespace {

void put_u32(std::string& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xFFULL));
  }
}

std::uint32_t read_u32(const unsigned char* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t read_u64(const unsigned char* bytes) {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8U) | static_cast<std::uint64_t>(bytes[index]);
  }
  return value;
}

std::string make_header(std::uint64_t writer_epoch, std::int64_t created_nanos) {
  std::string header;
  header.reserve(ThermalLog::kHeaderSize);
  header.append(ThermalLog::kMagic);
  put_u32(header, kLogFormatVersion);
  put_u32(header, static_cast<std::uint32_t>(ThermalLog::kHeaderSize));
  put_u64(header, writer_epoch);
  put_u64(header, static_cast<std::uint64_t>(created_nanos));
  put_u32(header, 0U);
  put_u32(header, 0U);
  put_u64(header, 0ULL);
  const std::uint32_t checksum = crc32c(std::string_view(header.data(), 48));
  put_u32(header, checksum);
  put_u32(header, 0U);
  put_u64(header, 0ULL);
  return header;
}

Status flush_file(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return Status::failure(ErrorCode::kIoFailure, "flushing the log failed");
  }
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    return Status::failure(ErrorCode::kIoFailure, "committing the log to the device failed");
  }
#else
  if (fsync(fileno(file)) != 0) {
    return Status::failure(ErrorCode::kIoFailure, "committing the log to the device failed");
  }
#endif
  return Status::success();
}

}  // namespace

ThermalLog::~ThermalLog() { (void)close(); }

ThermalLog::ThermalLog(ThermalLog&& other) noexcept
    : file_(other.file_), path_(std::move(other.path_)), limits_(other.limits_),
      load_(std::move(other.load_)), next_sequence_(other.next_sequence_),
      committed_bytes_(other.committed_bytes_), discarded_tail_bytes_(other.discarded_tail_bytes_),
      poisoned_(other.poisoned_) {
  other.file_ = nullptr;
  other.poisoned_ = false;
}

ThermalLog& ThermalLog::operator=(ThermalLog&& other) noexcept {
  if (this != &other) {
    (void)close();
    file_ = other.file_;
    path_ = std::move(other.path_);
    limits_ = other.limits_;
    load_ = std::move(other.load_);
    next_sequence_ = other.next_sequence_;
    committed_bytes_ = other.committed_bytes_;
    discarded_tail_bytes_ = other.discarded_tail_bytes_;
    poisoned_ = other.poisoned_;
    other.file_ = nullptr;
    other.poisoned_ = false;
  }
  return *this;
}

Result<ThermalLog::LoadResult> ThermalLog::inspect(const std::filesystem::path& path,
                                                   const Limits& limits) {
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    return Status::failure(ErrorCode::kIoFailure,
                           "the log path " + path.string() + " is a directory, not a log");
  }
  if (!std::filesystem::exists(path, error)) {
    return Status::failure(ErrorCode::kNotFound, "no durable log exists at " + path.string());
  }
  const auto file_size = std::filesystem::file_size(path, error);
  if (error) {
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot size the log at " + path.string() + ": " + error.message());
  }

  LoadResult result;
  result.file_bytes = file_size;

  if (file_size < kHeaderSize) {
    if (file_size != 0) {
      // A partial header means creation never committed. No record can exist behind it.
      result.reinitialised_short_header = true;
    } else {
      result.created = true;
    }
    return result;
  }

  Result<std::string> bytes = read_file_bytes(path, limits.max_segment_bytes);
  if (!bytes.ok()) {
    return bytes.error();
  }
  const std::string& content = bytes.value();
  const auto* raw = reinterpret_cast<const unsigned char*>(content.data());

  if (std::memcmp(content.data(), kMagic.data(), kMagic.size()) != 0) {
    return Status::failure(ErrorCode::kCorruptRecord,
                           "the log at " + path.string() + " does not begin with the expected magic");
  }
  const std::uint32_t version = read_u32(raw + 8);
  if (version != kLogFormatVersion) {
    return Status::failure(ErrorCode::kVersionMismatch,
                           "the log at " + path.string() + " is format version " +
                               std::to_string(version) + ", but this build reads version " +
                               std::to_string(kLogFormatVersion));
  }
  const std::uint32_t header_size = read_u32(raw + 12);
  if (header_size != kHeaderSize) {
    return Status::failure(ErrorCode::kCorruptRecord,
                           "the log header declares a size of " + std::to_string(header_size) +
                               " bytes, which this build does not understand");
  }
  const std::uint32_t header_crc = read_u32(raw + 48);
  const std::uint32_t computed = crc32c(std::string_view(content.data(), 48));
  if (header_crc != computed) {
    return Status::failure(ErrorCode::kIntegrityFailure,
                           "the log header at " + path.string() +
                               " fails its integrity check and is not trusted");
  }
  result.writer_epoch = read_u64(raw + 16);

  std::uint64_t offset = kHeaderSize;
  std::uint64_t sequence = 1;
  while (offset < file_size) {
    const std::uint64_t remaining = file_size - offset;
    if (remaining < kFrameOverhead) {
      result.discarded_tail_bytes = remaining;
      break;
    }
    const std::uint32_t payload_length = read_u32(raw + offset);
    const std::uint32_t payload_crc = read_u32(raw + offset + 4);
    const std::uint64_t frame_sequence = read_u64(raw + offset + 8);
    const std::uint64_t frame_end = offset + kFrameOverhead + payload_length;

    if (payload_length == 0) {
      if (frame_end >= file_size) {
        result.discarded_tail_bytes = remaining;
        break;
      }
      return Status::failure(ErrorCode::kCorruptRecord,
                             "the log at " + path.string() + " holds a zero-length frame at byte " +
                                 std::to_string(offset));
    }
    if (payload_length > limits.max_record_bytes) {
      if (frame_end > file_size) {
        result.discarded_tail_bytes = remaining;
        break;
      }
      return Status::failure(ErrorCode::kCorruptRecord,
                             "the log at " + path.string() + " holds a frame of " +
                                 std::to_string(payload_length) +
                                 " bytes, above the configured record bound");
    }
    if (frame_end > file_size) {
      // The frame cannot be complete: a torn tail.
      result.discarded_tail_bytes = remaining;
      break;
    }

    const std::uint32_t actual_crc =
        crc32c(std::string_view(content.data() + offset + kFrameOverhead, payload_length));
    if (actual_crc != payload_crc) {
      if (frame_end == file_size) {
        result.discarded_tail_bytes = remaining;
        break;
      }
      return Status::failure(
          ErrorCode::kIntegrityFailure,
          "the log at " + path.string() + " holds a corrupt frame at byte " +
              std::to_string(offset) + " with " + std::to_string(file_size - frame_end) +
              " bytes after it; refusing to load a log with interior damage");
    }

    if (frame_sequence != sequence) {
      return Status::failure(ErrorCode::kCorruptRecord,
                             "the log at " + path.string() + " jumps from sequence " +
                                 std::to_string(sequence) + " to " + std::to_string(frame_sequence));
    }
    if (result.payloads.size() >= limits.max_loaded_records) {
      return Status::failure(ErrorCode::kLimitExceeded,
                             "the log at " + path.string() + " holds more records than the "
                                 "configured load bound of " +
                                 std::to_string(limits.max_loaded_records));
    }
    result.payloads.emplace_back(content.data() + offset + kFrameOverhead, payload_length);
    offset = frame_end;
    ++sequence;
  }

  result.next_sequence = sequence;
  result.committed_bytes = offset;
  return result;
}

Status ThermalLog::open(const std::filesystem::path& path, OpenMode mode, std::uint64_t writer_epoch,
                        const Limits& limits) {
  if (is_open()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "the log at " + path_.string() + " is already open");
  }
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  const Status directory = ensure_directory(path.parent_path());
  if (!directory.ok()) {
    return directory;
  }

  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    return Status::failure(ErrorCode::kIoFailure,
                           "the log path " + path.string() + " is a directory, not a log");
  }
  const bool exists = std::filesystem::exists(path, error);
  if (mode == OpenMode::kOpenExisting && !exists) {
    return Status::failure(ErrorCode::kNotFound, "no durable log exists at " + path.string());
  }
  if (mode == OpenMode::kCreateNew && exists) {
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size >= kHeaderSize) {
      return Status::failure(ErrorCode::kDuplicateIdentity,
                             "a durable log already exists at " + path.string());
    }
    if (!error && size != 0) {
      const Status removed = remove_file_if_present(path);
      if (!removed.ok()) {
        return removed;
      }
    }
  }

  LoadResult recovered;
  bool need_header = !exists;
  if (exists) {
    Result<LoadResult> inspected = inspect(path, limits);
    if (!inspected.ok()) {
      return inspected.error();
    }
    recovered = inspected.value();
    if (recovered.reinitialised_short_header || recovered.file_bytes == 0) {
      // A file that exists but holds nothing has no header: it was created by something other than
      // a completed header write, or the header write never reached the device. Treating it as a
      // log would make the first commit overwrite the magic and leave the store unreadable for
      // ever, so it is re-initialised instead.
      need_header = true;
      const Status removed = remove_file_if_present(path);
      if (!removed.ok()) {
        return removed;
      }
      recovered = LoadResult{};
    } else {
      next_sequence_ = recovered.next_sequence;
      committed_bytes_ = recovered.committed_bytes;
      discarded_tail_bytes_ = recovered.discarded_tail_bytes;
    }
  }

  std::FILE* file = open_stream(path, need_header ? "w+b" : "r+b");
  if (file == nullptr) {
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot open the log at " + path.string() + " for writing");
  }

  if (need_header) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const std::string header = make_header(writer_epoch, nanos);
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) {
      std::fclose(file);
      return Status::failure(ErrorCode::kIoFailure,
                             "cannot write the log header at " + path.string());
    }
    const Status flushed = flush_file(file);
    if (!flushed.ok()) {
      std::fclose(file);
      return flushed;
    }
    next_sequence_ = 1;
    committed_bytes_ = kHeaderSize;
    discarded_tail_bytes_ = 0;
    recovered = LoadResult{};
    recovered.created = !exists;
    recovered.file_bytes = kHeaderSize;
    recovered.committed_bytes = kHeaderSize;
    recovered.next_sequence = 1;
  } else if (discarded_tail_bytes_ > 0) {
    // Truncate the torn tail so that the next append starts at a committed offset. The committed
    // prefix is untouched, so this cannot lose a committed record.
    const Status positioned = stream_seek(file, committed_bytes_);
    if (!positioned.ok()) {
      std::fclose(file);
      return Status::failure(ErrorCode::kIoFailure, "cannot seek the log to its commit point");
    }
    const Status truncated = stream_truncate(file, committed_bytes_);
    if (!truncated.ok()) {
      std::fclose(file);
      return Status::failure(ErrorCode::kIoFailure, "cannot truncate the torn log tail");
    }
  }

  // The file is exactly as long as its committed prefix at this point: a torn tail, if there was
  // one, has just been truncated away. Seeking past it would leave a hole that the next append
  // would fill, which is precisely the interior corruption this class exists to prevent.
  const Status at_end = stream_seek(file, committed_bytes_);
  if (!at_end.ok()) {
    std::fclose(file);
    return Status::failure(ErrorCode::kIoFailure, "cannot position the log at its end");
  }
  file_ = file;
  path_ = path;
  limits_ = limits;
  load_ = recovered;
  return Status::success();
}

Status ThermalLog::close() {
  if (file_ == nullptr) {
    return Status::success();
  }
  std::FILE* file = static_cast<std::FILE*>(file_);
  const Status flushed = flush_file(file);
  std::fclose(file);
  file_ = nullptr;
  return flushed;
}

Result<std::uint64_t> ThermalLog::append(std::string_view payload, bool durable) {
  if (file_ == nullptr) {
    return Status::failure(ErrorCode::kClosed, "the log is not open");
  }
  if (poisoned_) {
    return Status::failure(
        ErrorCode::kIoFailure,
        "the log is closed to further appends after a failed write; reopen it so that the "
        "uncommitted tail is trimmed and the next commit starts at a committed offset");
  }
  if (payload.empty()) {
    return Status::failure(ErrorCode::kInvalidArgument, "an empty record cannot be appended");
  }
  if (payload.size() > limits_.max_record_bytes) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           "a record of " + std::to_string(payload.size()) +
                               " bytes exceeds the configured bound of " +
                               std::to_string(limits_.max_record_bytes));
  }
  const std::uint64_t frame_end = committed_bytes_ + kFrameOverhead + payload.size();
  if (frame_end > limits_.max_segment_bytes) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           "appending would grow the segment past the configured bound of " +
                               std::to_string(limits_.max_segment_bytes) + " bytes");
  }

  std::string frame;
  frame.reserve(kFrameOverhead + payload.size());
  put_u32(frame, static_cast<std::uint32_t>(payload.size()));
  put_u32(frame, crc32c(payload));
  put_u64(frame, next_sequence_);
  frame.append(payload);

  std::FILE* file = static_cast<std::FILE*>(file_);
  const std::size_t written = std::fwrite(frame.data(), 1, frame.size(), file);
  if (written != frame.size()) {
    // A short write leaves a torn frame and advances the file position past it. Appending again
    // from there would place a valid frame after a torn one, which is interior corruption rather
    // than a recoverable tail. The tail is therefore rolled back to the last committed offset and
    // the log refuses further appends until it is reopened.
    roll_back_tail();
    poisoned_ = true;
    return Status::failure(ErrorCode::kIoFailure,
                           "a short write occurred; the frame was not committed and the log has "
                           "been closed to further appends until it is reopened");
  }
  if (durable) {
    const Status flushed = flush_file(file);
    if (!flushed.ok()) {
      // The bytes are in the operating system's hands but the device has not confirmed them, so
      // they are not committed. Rolling back keeps the on-disk tail at a committed offset.
      roll_back_tail();
      poisoned_ = true;
      return flushed;
    }
  }
  const std::uint64_t sequence = next_sequence_;
  ++next_sequence_;
  committed_bytes_ = frame_end;
  return sequence;
}

void ThermalLog::roll_back_tail() {
  if (file_ == nullptr) {
    return;
  }
  std::FILE* file = static_cast<std::FILE*>(file_);
  if (!stream_seek(file, committed_bytes_).ok()) {
    return;
  }
  if (!stream_truncate(file, committed_bytes_).ok()) {
    return;
  }
  (void)std::fflush(file);
}

Status ThermalLog::flush() {
  if (file_ == nullptr) {
    return Status::failure(ErrorCode::kClosed, "the log is not open");
  }
  return flush_file(static_cast<std::FILE*>(file_));
}

}  // namespace tobsv