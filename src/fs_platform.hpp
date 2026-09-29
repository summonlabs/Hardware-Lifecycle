#pragma once

// Hardware Lifecycle - internal platform layer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This header is not installed. It is the only place in the repository that
// includes a platform SDK header: everything above it sees Result, paths and
// byte spans.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle::detail {

/// Validates one caller supplied path segment: rejects empty, ".", "..", any
/// separator, any Windows alternate data stream colon, reserved device names,
/// trailing dot or space, control characters and NUL.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> validate_path_segment(std::string_view segment);

/// Joins a validated segment onto a root directory.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::filesystem::path> safe_child_path(const std::filesystem::path& root,
                                                            std::string_view segment);

/// Creates the directory and every missing parent. Succeeds when it exists.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> ensure_directory(const std::filesystem::path& path);

[[nodiscard]] HARDWARE_LIFECYCLE_API Result<bool> path_exists(const std::filesystem::path& path);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<bool> is_directory(const std::filesystem::path& path);

/// True when the path is a symbolic link, junction or other reparse point. The
/// store refuses to operate through one.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<bool> is_reparse_point(const std::filesystem::path& path);

[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::uint64_t> file_size(const std::filesystem::path& path);

/// Sorted file names (not full paths) of a directory. Sorted so that every
/// caller is deterministic.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::vector<std::string>> list_directory(const std::filesystem::path& path);

[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> remove_file(const std::filesystem::path& path);

/// Reads a whole file. A file longer than max_bytes is a LimitExceeded; it is
/// never truncated.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes);

/// Writes to a temporary sibling, flushes it, then atomically replaces the
/// target. With durable set the temporary is flushed to the device before the
/// replace and the replace itself is write through. The temporary is removed on
/// every failure path.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> write_file_atomic(const std::filesystem::path& target,
                                             std::span<const std::uint8_t> bytes, bool durable);

/// An append oriented file handle with explicit device flush and truncation.
class AppendFile {
 public:
  AppendFile() noexcept;
  ~AppendFile();
  AppendFile(AppendFile&& other) noexcept;
  AppendFile& operator=(AppendFile&& other) noexcept;
  AppendFile(const AppendFile&) = delete;
  AppendFile& operator=(const AppendFile&) = delete;

  /// Opens for append, creating the file when missing.
  [[nodiscard]] static Result<AppendFile> open(const std::filesystem::path& path);

  [[nodiscard]] Result<void> append(std::span<const std::uint8_t> bytes);
  [[nodiscard]] Result<void> flush_to_device();
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_range(std::uint64_t offset, std::size_t size) const;
  [[nodiscard]] Result<std::uint64_t> size() const;
  [[nodiscard]] Result<void> truncate_to(std::uint64_t size);
  [[nodiscard]] bool is_open() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// An operating system level exclusive lock on one file.
///
/// The lock lives on the handle, so it is released by the operating system when
/// the process dies, whether or not any destructor ran. That property is a proof
/// obligation and is exercised by a real two process test.
class ExclusiveFileLock {
 public:
  ExclusiveFileLock() noexcept;
  ~ExclusiveFileLock();
  ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
  ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;
  ExclusiveFileLock(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

  /// Fails with StoreLocked when another live process holds the lock.
  [[nodiscard]] static Result<ExclusiveFileLock> acquire(const std::filesystem::path& path);

  [[nodiscard]] bool held() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Process id, used in diagnostics only. Never used as an identity.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::uint64_t current_process_id() noexcept;

}  // namespace hardware_lifecycle::detail
