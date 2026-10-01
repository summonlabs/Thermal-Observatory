// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/persistence/files.hpp"

#include <cstdio>
#include <string>

#include <cstdint>

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace tobsv {
namespace {

Status flush_to_device(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return Status::failure(ErrorCode::kIoFailure, "flushing a file failed");
  }
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    return Status::failure(ErrorCode::kIoFailure, "committing a file to the device failed");
  }
#else
  if (fsync(fileno(file)) != 0) {
    return Status::failure(ErrorCode::kIoFailure, "committing a file to the device failed");
  }
#endif
  return Status::success();
}

}  // namespace

std::FILE* open_stream(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
  return ::_fsopen(path.string().c_str(), mode, _SH_DENYNO);
#else
  return std::fopen(path.string().c_str(), mode);
#endif
}

Result<std::uint64_t> stream_size(std::FILE* file) {
  if (file == nullptr) {
    return Status::failure(ErrorCode::kInvalidArgument, "no stream was supplied");
  }
#if defined(_WIN32)
  if (_fseeki64(file, 0, SEEK_END) != 0) {
#else
  if (fseeko(file, 0, SEEK_END) != 0) {
#endif
    return Status::failure(ErrorCode::kIoFailure, "cannot seek to the end of the stream");
  }
#if defined(_WIN32)
  const __int64 position = _ftelli64(file);
#else
  const off_t position = ftello(file);
#endif
  if (position < 0) {
    return Status::failure(ErrorCode::kIoFailure, "cannot determine the stream position");
  }
  return static_cast<std::uint64_t>(position);
}

Status stream_seek(std::FILE* file, std::uint64_t offset) {
  if (file == nullptr) {
    return Status::failure(ErrorCode::kInvalidArgument, "no stream was supplied");
  }
  if (offset > static_cast<std::uint64_t>(INT64_MAX)) {
    return Status::failure(ErrorCode::kOutOfRange, "the requested offset is out of range");
  }
#if defined(_WIN32)
  if (_fseeki64(file, static_cast<__int64>(offset), SEEK_SET) != 0) {
#else
  if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
    return Status::failure(ErrorCode::kIoFailure, "cannot seek the stream");
  }
  return Status::success();
}

Status stream_truncate(std::FILE* file, std::uint64_t size) {
  if (file == nullptr) {
    return Status::failure(ErrorCode::kInvalidArgument, "no stream was supplied");
  }
#if defined(_WIN32)
  if (_chsize_s(_fileno(file), size) != 0) {
#else
  if (ftruncate(fileno(file), static_cast<off_t>(size)) != 0) {
#endif
    return Status::failure(ErrorCode::kIoFailure, "cannot truncate the stream");
  }
  return Status::success();
}

Result<std::string> read_file_bytes(const std::filesystem::path& path, std::size_t max_bytes) {
  // A directory is not a file, and the platforms disagree about what happens if you treat one as
  // one: Windows fails at the open, while Linux opens it successfully and fails on the first read,
  // after reporting the directory's own size as though it were the file's. Refusing here keeps the
  // outcome identical everywhere and stops a size bound from being reported for something that was
  // never a file.
  std::error_code kind_error;
  if (std::filesystem::is_directory(path, kind_error)) {
    return Status::failure(ErrorCode::kIoFailure,
                           path.string() + " is a directory, not a file");
  }
  std::FILE* file = open_stream(path, "rb");
  if (file == nullptr) {
    return Status::failure(ErrorCode::kNotFound,
                           "cannot open " + path.string() + " for reading");
  }
  const Result<std::uint64_t> size = stream_size(file);
  if (!size.ok()) {
    std::fclose(file);
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot size " + path.string() + ": " + size.error().detail());
  }
  if (size.value() > max_bytes) {
    std::fclose(file);
    return Status::failure(ErrorCode::kLimitExceeded,
                           path.string() + " is larger than the accepted bound of " +
                               std::to_string(max_bytes) + " bytes");
  }
  const Status rewind_status = stream_seek(file, 0);
  if (!rewind_status.ok()) {
    std::fclose(file);
    return Status::failure(ErrorCode::kIoFailure, "cannot rewind " + path.string());
  }

  std::string bytes;
  bytes.resize(static_cast<std::size_t>(size.value()));
  if (size.value() > 0) {
    const std::size_t read =
        std::fread(bytes.data(), 1, static_cast<std::size_t>(size.value()), file);
    if (read != static_cast<std::size_t>(size.value())) {
      std::fclose(file);
      return Status::failure(ErrorCode::kIoFailure, "short read from " + path.string());
    }
  }
  std::fclose(file);
  return bytes;
}

Status write_file_atomic(const std::filesystem::path& path, std::string_view bytes) {
  // The fallback below removes the destination before renaming. If the destination were a
  // directory, that would delete it, so a directory destination is refused outright.
  std::error_code kind_error;
  if (std::filesystem::is_directory(path, kind_error)) {
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot publish to " + path.string() + ": it is a directory");
  }
  const std::filesystem::path temporary = path.string() + ".tmp";
  std::FILE* file = open_stream(temporary, "wb");
  if (file == nullptr) {
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot open " + temporary.string() + " for writing");
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size()) {
      std::fclose(file);
      (void)std::remove(temporary.string().c_str());
      return Status::failure(ErrorCode::kIoFailure, "short write to " + temporary.string());
    }
  }
  const Status flushed = flush_to_device(file);
  std::fclose(file);
  if (!flushed.ok()) {
    (void)std::remove(temporary.string().c_str());
    return flushed;
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    // A rename over an existing file can fail on some platforms; fall back to remove plus rename,
    // which is still not observable as a partial file because the temporary is complete.
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
      (void)std::remove(temporary.string().c_str());
      return Status::failure(ErrorCode::kIoFailure,
                             "cannot publish " + path.string() + ": " + error.message());
    }
  }
  return Status::success();
}

Status ensure_directory(const std::filesystem::path& path) {
  if (path.empty()) {
    return Status::success();
  }
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    return Status::success();
  }
  std::filesystem::create_directories(path, error);
  if (error && !std::filesystem::is_directory(path)) {
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot create directory " + path.string() + ": " + error.message());
  }
  return Status::success();
}

Status remove_file_if_present(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error && std::filesystem::exists(path)) {
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot remove " + path.string() + ": " + error.message());
  }
  return Status::success();
}

}  // namespace tobsv