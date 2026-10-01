// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "tobsv/core/result.hpp"

namespace tobsv {

// Opens a stream allowing other handles to read and write the same file.
//
// This matters on Windows: the CRT's own sharing default for fopen_s denies other handles, which
// would stop the runtime from inspecting its own log while its writer holds it open, and would stop
// an operator's read-only tooling from looking at a live store. Exclusion is not the file sharing
// mode's job here; the kernel single-writer lock is what excludes a second writer, and reads are
// deliberately still possible. Returns nullptr when the file cannot be opened.
std::FILE* open_stream(const std::filesystem::path& path, const char* mode);

// 64-bit stream position helpers. The C runtime's ftell and fseek are 32-bit on Windows, so a
// segment larger than 2 GiB - which the configured bounds permit - could be written and then never
// read back. Everything in this runtime goes through these instead.
// Seeks to the end of the stream and reports its length. The seek is part of the contract so that
// a caller cannot ask for the size of a stream whose position was never moved to the end.
Result<std::uint64_t> stream_size(std::FILE* file);
Status stream_seek(std::FILE* file, std::uint64_t offset);
Status stream_truncate(std::FILE* file, std::uint64_t size);

// Reads a whole file, refusing anything larger than the caller's bound. A missing file is a
// failure, not an empty result.
Result<std::string> read_file_bytes(const std::filesystem::path& path, std::size_t max_bytes);

// Publishes bytes atomically: a temporary file in the same directory is written, flushed to the
// device and then renamed over the destination. A reader therefore sees either the previous
// complete content or the new complete content, never a mixture.
Status write_file_atomic(const std::filesystem::path& path, std::string_view bytes);

// Creates parent directories when they are missing. Succeeds when they already exist.
Status ensure_directory(const std::filesystem::path& path);

// Best-effort removal used for temporary artefacts. A missing file is not an error.
Status remove_file_if_present(const std::filesystem::path& path);

}  // namespace tobsv
