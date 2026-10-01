// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/persistence/lock.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace tobsv {
namespace {

long current_process_id() {
#if defined(_WIN32)
  return static_cast<long>(::GetCurrentProcessId());
#else
  return static_cast<long>(::getpid());
#endif
}

}  // namespace

SingleWriterLock::~SingleWriterLock() { release(); }

SingleWriterLock::SingleWriterLock(SingleWriterLock&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)), owner_pid_(other.owner_pid_) {
  other.handle_ = nullptr;
  other.owner_pid_ = 0;
}

SingleWriterLock& SingleWriterLock::operator=(SingleWriterLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    owner_pid_ = other.owner_pid_;
    other.handle_ = nullptr;
    other.owner_pid_ = 0;
  }
  return *this;
}

Status SingleWriterLock::acquire(const std::filesystem::path& path) {
  if (held()) {
    return Status::failure(ErrorCode::kLocked,
                           "this handle already holds the lock on " + path_);
  }
  const std::string native = path.string();
#if defined(_WIN32)
  // FILE_SHARE_READ keeps the owner identity readable for diagnosis; the exclusion comes from the
  // byte-range lock below, which the kernel enforces against every other opener.
  HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot open lock file " + native + " (windows error " +
                               std::to_string(static_cast<unsigned long>(error)) + ")");
  }
  OVERLAPPED overlapped{};
  if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &overlapped) == 0) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
      return Status::failure(ErrorCode::kLocked,
                             "lock file " + native + " is already held by another writer");
    }
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot lock " + native + " (windows error " +
                               std::to_string(static_cast<unsigned long>(error)) + ")");
  }
  const long pid = current_process_id();
  const std::string owner = "owner_pid=" + std::to_string(pid) + "\n";
  DWORD written = 0;
  (void)::SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
  (void)::WriteFile(handle, owner.data(), static_cast<DWORD>(owner.size()), &written, nullptr);
  (void)::FlushFileBuffers(handle);
  handle_ = handle;
#else
  const int descriptor = ::open(native.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot open lock file " + native + ": " + std::generic_category().message(errno));
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    ::close(descriptor);
    if (error == EWOULDBLOCK || error == EAGAIN) {
      return Status::failure(ErrorCode::kLocked,
                             "lock file " + native + " is already held by another writer");
    }
    return Status::failure(ErrorCode::kIoFailure,
                           "cannot lock " + native + ": " + std::generic_category().message(error));
  }
  const long pid = current_process_id();
  const std::string owner = "owner_pid=" + std::to_string(pid) + "\n";
  if (::ftruncate(descriptor, 0) == 0) {
    ssize_t ignored = ::write(descriptor, owner.data(), owner.size());
    (void)ignored;
    (void)::fsync(descriptor);
  }
  handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(descriptor + 1));
#endif
  path_ = native;
  owner_pid_ = current_process_id();
  return Status::success();
}

bool SingleWriterLock::held() const noexcept { return handle_ != nullptr; }

void SingleWriterLock::release() {
  if (!held()) {
    return;
  }
#if defined(_WIN32)
  HANDLE handle = static_cast<HANDLE>(handle_);
  OVERLAPPED overlapped{};
  (void)::UnlockFileEx(handle, 0, 1, 0, &overlapped);
  (void)::CloseHandle(handle);
#else
  const int descriptor = static_cast<int>(reinterpret_cast<intptr_t>(handle_)) - 1;
  (void)::flock(descriptor, LOCK_UN);
  (void)::close(descriptor);
#endif
  handle_ = nullptr;
  owner_pid_ = 0;
}

}  // namespace tobsv