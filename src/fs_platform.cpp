// Hardware Lifecycle - internal platform layer implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This is the only translation unit in the repository that includes a platform
// SDK header. Everything above it works with Result, std::filesystem::path and
// byte spans.
//
// Invariants this file exists to uphold:
//  - nothing is repaired. A malformed name is rejected, a short read is an
//    integrity failure, an absent file never decays into an empty one, and a
//    bound is never satisfied by truncation,
//  - no operation follows a reparse point. The refusal is decided on the handle
//    that was actually opened, so a link swapped in between a query and an open
//    cannot be traversed,
//  - an atomic replace either publishes the complete new content or leaves the
//    previous content untouched, and it never leaves its temporary behind,
//  - an exclusive lock is a property of the operating system handle: the kernel
//    releases it when the process dies, with no destructor, cleanup handler or
//    recovery pass involved.
//
// Missing, unknown and unmeasured are three different facts and are reported as
// three different results. Where a query can answer "absent" it does so; every
// other failure is propagated with the operating system code as a note.

#include "fs_platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "hardware_lifecycle/limits.hpp"

#if defined(_WIN32)

#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#else

#  include <cerrno>
#  include <climits>
#  include <cstdio>
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

#endif

namespace hardware_lifecycle::detail {

namespace {

// ---------------------------------------------------------------------------
// Helpers shared by both platforms.
// ---------------------------------------------------------------------------

/// ASCII upper case. Device names are ASCII, so a byte above 0x7F can never be
/// part of one and is left alone.
[[nodiscard]] constexpr unsigned char ascii_upper(unsigned char value) noexcept {
  if (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) {
    return static_cast<unsigned char>(value - static_cast<unsigned char>('a') + static_cast<unsigned char>('A'));
  }
  return value;
}

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const unsigned char left_byte = ascii_upper(static_cast<unsigned char>(left[index]));
    const unsigned char right_byte = ascii_upper(static_cast<unsigned char>(right[index]));
    if (left_byte != right_byte) {
      return false;
    }
  }
  return true;
}

/// True when the name denotes a character device rather than a file. The stem
/// before the first '.' is what the operating system resolves, so "con.txt" and
/// "NUL" are the same device, and a trailing space in the stem is ignored
/// because the Win32 name resolver ignores it too.
[[nodiscard]] bool is_reserved_device_name(std::string_view segment) noexcept {
  constexpr std::string_view kReserved[] = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5",
      "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
      "LPT6", "LPT7", "LPT8", "LPT9",
  };
  const std::size_t dot = segment.find('.');
  std::string_view stem = dot == std::string_view::npos ? segment : segment.substr(0, dot);
  while (!stem.empty() && stem.back() == ' ') {
    stem.remove_suffix(1);
  }
  for (const std::string_view reserved : kReserved) {
    if (ascii_iequals(stem, reserved)) {
      return true;
    }
  }
  return false;
}

/// Number of UTF-16 code units the text occupies, decoding UTF-8 strictly.
/// Overlong forms, surrogates, values above U+10FFFF and truncated sequences
/// are rejected: the platform conversion would otherwise substitute U+FFFD and
/// silently name a different file.
[[nodiscard]] Result<std::size_t> utf16_unit_count(std::string_view text) {
  std::size_t units = 0;
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (lead < 0x80) {
      length = 1;
      code_point = lead;
      minimum = 0;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
      code_point = static_cast<std::uint32_t>(lead & 0x1F);
      minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      code_point = static_cast<std::uint32_t>(lead & 0x0F);
      minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      code_point = static_cast<std::uint32_t>(lead & 0x07);
      minimum = 0x10000;
    } else {
      return fail(ErrorCode::MalformedRequest, "path segment is not well formed UTF-8");
    }
    if (text.size() - index < length) {
      return fail(ErrorCode::MalformedRequest, "path segment ends inside a UTF-8 sequence");
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0) != 0x80) {
        return fail(ErrorCode::MalformedRequest, "path segment is not well formed UTF-8");
      }
      code_point = (code_point << 6) | static_cast<std::uint32_t>(continuation & 0x3F);
    }
    if (code_point < minimum) {
      return fail(ErrorCode::MalformedRequest, "path segment uses an overlong UTF-8 encoding");
    }
    if (code_point > 0x10FFFF) {
      return fail(ErrorCode::MalformedRequest, "path segment contains a code point above U+10FFFF");
    }
    if (code_point >= 0xD800 && code_point <= 0xDFFF) {
      return fail(ErrorCode::MalformedRequest, "path segment contains a UTF-16 surrogate code point");
    }
    units += code_point >= 0x10000 ? 2 : 1;
    index += length;
  }
  return units;
}

/// Next temporary file suffix. The counter is process local and monotonic, so
/// two writes in one process can never choose the same name; the process id in
/// the name keeps two processes apart.
[[nodiscard]] Result<std::uint64_t> next_temp_counter() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t value = counter.fetch_add(1, std::memory_order_relaxed);
  if (value == std::numeric_limits<std::uint64_t>::max()) {
    return fail(ErrorCode::CounterOverflow, "the temporary file counter is exhausted");
  }
  return value + 1;
}

}  // namespace

#if defined(_WIN32)

namespace {

/// Documented ceiling of the extended length path form, in UTF-16 code units.
constexpr std::size_t kExtendedPathLimit = 32767;

/// Largest single read or write request. The Win32 entry points take a 32 bit
/// count, so a larger payload is split into several calls, never truncated.
constexpr std::size_t kMaxIoChunk = 1u << 30;

/// Deterministic spelling of a Win32 error. The system message text is
/// deliberately not used: it is localised, and a diagnostic that changes with
/// the display language is not a stable diagnostic.
[[nodiscard]] std::string win32_detail(DWORD code) {
  std::string detail = std::to_string(static_cast<unsigned long long>(code));
  const char* name = nullptr;
  switch (code) {
    case ERROR_FILE_NOT_FOUND:
      name = "ERROR_FILE_NOT_FOUND";
      break;
    case ERROR_PATH_NOT_FOUND:
      name = "ERROR_PATH_NOT_FOUND";
      break;
    case ERROR_ACCESS_DENIED:
      name = "ERROR_ACCESS_DENIED";
      break;
    case ERROR_INVALID_HANDLE:
      name = "ERROR_INVALID_HANDLE";
      break;
    case ERROR_INVALID_NAME:
      name = "ERROR_INVALID_NAME";
      break;
    case ERROR_BAD_PATHNAME:
      name = "ERROR_BAD_PATHNAME";
      break;
    case ERROR_WRITE_PROTECT:
      name = "ERROR_WRITE_PROTECT";
      break;
    case ERROR_SHARING_VIOLATION:
      name = "ERROR_SHARING_VIOLATION";
      break;
    case ERROR_LOCK_VIOLATION:
      name = "ERROR_LOCK_VIOLATION";
      break;
    case ERROR_HANDLE_DISK_FULL:
      name = "ERROR_HANDLE_DISK_FULL";
      break;
    case ERROR_DISK_FULL:
      name = "ERROR_DISK_FULL";
      break;
    case ERROR_ALREADY_EXISTS:
      name = "ERROR_ALREADY_EXISTS";
      break;
    case ERROR_FILE_EXISTS:
      name = "ERROR_FILE_EXISTS";
      break;
    case ERROR_DIR_NOT_EMPTY:
      name = "ERROR_DIR_NOT_EMPTY";
      break;
    case ERROR_NEGATIVE_SEEK:
      name = "ERROR_NEGATIVE_SEEK";
      break;
    case ERROR_IO_DEVICE:
      name = "ERROR_IO_DEVICE";
      break;
    case ERROR_NOT_SAME_DEVICE:
      name = "ERROR_NOT_SAME_DEVICE";
      break;
    case ERROR_PRIVILEGE_NOT_HELD:
      name = "ERROR_PRIVILEGE_NOT_HELD";
      break;
    default:
      break;
  }
  if (name != nullptr) {
    detail += " (";
    detail += name;
    detail += ")";
  }
  return detail;
}

/// Maps a Win32 error onto the taxonomy. The fallback is used for codes that
/// carry no more specific meaning.
[[nodiscard]] Error win32_failure(ErrorCode fallback, DWORD code, std::string_view operation) {
  ErrorCode mapped = fallback;
  switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
      mapped = ErrorCode::ObjectNotFound;
      break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
      mapped = ErrorCode::PermissionDenied;
      break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
      mapped = ErrorCode::ObjectExists;
      break;
    case ERROR_LOCK_VIOLATION:
    case ERROR_SHARING_VIOLATION:
      mapped = ErrorCode::StoreLocked;
      break;
    default:
      break;
  }
  Error error = fail(mapped, std::string(operation) + " failed");
  return error.with("win32_error", win32_detail(code));
}

[[nodiscard]] Error from_error_code(const std::error_code& code, std::string_view operation) {
  if (code.category() == std::system_category()) {
    return win32_failure(ErrorCode::IoFailure, static_cast<DWORD>(code.value()), operation);
  }
  ErrorCode mapped = ErrorCode::IoFailure;
  if (code == std::errc::permission_denied) {
    mapped = ErrorCode::PermissionDenied;
  } else if (code == std::errc::no_such_file_or_directory) {
    mapped = ErrorCode::ObjectNotFound;
  } else if (code == std::errc::file_exists) {
    mapped = ErrorCode::ObjectExists;
  } else if (code == std::errc::not_a_directory) {
    mapped = ErrorCode::UnsupportedOperation;
  }
  Error error = fail(mapped, std::string(operation) + " failed");
  return error.with("system_error", "value=" + std::to_string(code.value()) + " category=" + code.category().name());
}

/// Absence of an object and failure of the query that looked for it are
/// different facts, so they are different results.
struct Lookup {
  DWORD attributes = 0;
  bool present = false;
};

[[nodiscard]] Result<Lookup> lookup_object(const std::filesystem::path& path, std::string_view operation) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    Lookup found;
    found.attributes = attributes;
    found.present = true;
    return found;
  }
  const DWORD code = GetLastError();
  switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
      // A name the file system cannot represent cannot denote an existing
      // object, so "absent" is a fact here rather than a guess.
      return Lookup{};
    default:
      return win32_failure(ErrorCode::IoFailure, code, operation);
  }
}

/// Attributes of an object that must exist. A missing object is ObjectNotFound
/// and never a default constructed value.
[[nodiscard]] Result<DWORD> attributes_of(const std::filesystem::path& path, std::string_view operation) {
  const Result<Lookup> found = lookup_object(path, operation);
  if (!found) {
    return found.error();
  }
  if (!found.value().present) {
    return fail(ErrorCode::ObjectNotFound, std::string(operation) + ": the object does not exist");
  }
  return found.value().attributes;
}

/// Closes a file handle on every exit path. Closing the handle is also what
/// releases a byte range lock.
class HandleGuard {
 public:
  HandleGuard() noexcept = default;
  explicit HandleGuard(HANDLE handle) noexcept : handle_(handle) {}
  ~HandleGuard() { reset(); }

  HandleGuard(HandleGuard&& other) noexcept : handle_(other.release()) {}
  HandleGuard& operator=(HandleGuard&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.release();
    }
    return *this;
  }
  HandleGuard(const HandleGuard&) = delete;
  HandleGuard& operator=(const HandleGuard&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

  HANDLE release() noexcept {
    const HANDLE released = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return released;
  }

  void reset() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

/// Closes a directory search on every exit path.
class SearchGuard {
 public:
  explicit SearchGuard(HANDLE search) noexcept : search_(search) {}
  ~SearchGuard() { reset(); }
  SearchGuard(SearchGuard&& other) noexcept : search_(other.release()) {}
  SearchGuard& operator=(SearchGuard&& other) noexcept {
    if (this != &other) {
      reset();
      search_ = other.release();
    }
    return *this;
  }
  SearchGuard(const SearchGuard&) = delete;
  SearchGuard& operator=(const SearchGuard&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return search_; }

  HANDLE release() noexcept {
    const HANDLE released = search_;
    search_ = INVALID_HANDLE_VALUE;
    return released;
  }

  void reset() noexcept {
    if (search_ != INVALID_HANDLE_VALUE) {
      FindClose(search_);
      search_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  HANDLE search_ = INVALID_HANDLE_VALUE;
};

/// Opens an existing object and verifies on the handle itself that it is a
/// regular file and not a reparse point. Verifying after the open is what keeps
/// the guarantee true against a link swapped in after a separate query.
[[nodiscard]] Result<HandleGuard> open_regular_file(const std::filesystem::path& path, DWORD access, DWORD share,
                                                    std::string_view operation) {
  HandleGuard handle(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!handle.valid()) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), operation);
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle.get(), &information)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), operation);
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrorCode::ReparsePointRejected,
                std::string(operation) + " refuses to operate through a reparse point");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(ErrorCode::UnsupportedOperation, std::string(operation) + " requires a regular file");
  }
  return std::move(handle);
}

[[nodiscard]] Result<std::uint64_t> handle_size(HANDLE handle, std::string_view operation) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), operation);
  }
  if (size.QuadPart < 0) {
    return fail(ErrorCode::IoFailure, std::string(operation) + ": the file system reported a negative size");
  }
  return static_cast<std::uint64_t>(size.QuadPart);
}

/// Writes every byte or reports failure. A short write is resumed, never
/// accepted as completion.
[[nodiscard]] Result<void> write_all(HANDLE handle, std::span<const std::uint8_t> bytes, std::string_view operation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const DWORD chunk = static_cast<DWORD>(remaining < kMaxIoChunk ? remaining : kMaxIoChunk);
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr)) {
      return win32_failure(ErrorCode::IoFailure, GetLastError(), operation);
    }
    if (written == 0) {
      return fail(ErrorCode::IoFailure, std::string(operation) + " made no progress");
    }
    offset += static_cast<std::size_t>(written);
  }
  return ok();
}

/// Reads exactly the buffer length, or reports IntegrityFailure. A premature
/// end of file is a torn or truncated file, never a short but valid read.
[[nodiscard]] Result<void> read_exact(HANDLE handle, std::span<std::uint8_t> buffer, std::string_view operation) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const std::size_t remaining = buffer.size() - offset;
    const DWORD chunk = static_cast<DWORD>(remaining < kMaxIoChunk ? remaining : kMaxIoChunk);
    DWORD obtained = 0;
    if (!ReadFile(handle, buffer.data() + offset, chunk, &obtained, nullptr)) {
      return win32_failure(ErrorCode::IoFailure, GetLastError(), operation);
    }
    if (obtained == 0) {
      return fail(ErrorCode::IntegrityFailure, std::string(operation) + ": the file ended inside the requested range")
          .with("expected_bytes", std::to_string(buffer.size()))
          .with("read_bytes", std::to_string(offset));
    }
    offset += static_cast<std::size_t>(obtained);
  }
  return ok();
}

/// Decimal spelling that does not depend on the locale.
[[nodiscard]] std::wstring decimal_text(std::uint64_t value) {
  wchar_t digits[20] = {};
  std::size_t length = 0;
  do {
    digits[length] = static_cast<wchar_t>(L'0' + static_cast<wchar_t>(value % 10));
    ++length;
    value /= 10;
  } while (value != 0);
  std::wstring text(length, L'0');
  for (std::size_t index = 0; index < length; ++index) {
    text[index] = digits[length - 1 - index];
  }
  return text;
}

[[nodiscard]] std::wstring temp_suffix(std::uint64_t counter) {
  return L".tmp-" + decimal_text(current_process_id()) + L"-" + decimal_text(counter);
}

/// Converts one already validated path segment from UTF-8 to UTF-16. The
/// conversion is still checked, because a silent U+FFFD substitution would
/// name a different file.
[[nodiscard]] Result<std::wstring> to_wide(std::string_view text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int length = static_cast<int>(text.size());
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
  if (needed <= 0) {
    return fail(ErrorCode::MalformedRequest, "path segment is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), needed);
  if (written != needed) {
    return fail(ErrorCode::MalformedRequest, "path segment could not be converted to UTF-16");
  }
  return wide;
}

/// Converts one directory entry name to UTF-8. A name that cannot be encoded is
/// reported instead of being replaced with something that is not the name.
[[nodiscard]] Result<std::string> to_utf8(std::wstring_view text) {
  if (text.empty()) {
    return std::string();
  }
  const int length = static_cast<int>(text.size());
  const int needed =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return fail(ErrorCode::IoFailure, "a directory entry name is not valid UTF-16");
  }
  std::string encoded(static_cast<std::size_t>(needed), '\0');
  const int written =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, encoded.data(), needed, nullptr, nullptr);
  if (written != needed) {
    return fail(ErrorCode::IoFailure, "a directory entry name could not be encoded as UTF-8");
  }
  return encoded;
}

/// Extended length form of an absolute path. That form switches off every
/// normalisation the Win32 layer would otherwise apply, so the path is
/// normalised first and forward slashes, which are not separators in that form,
/// are converted to backslashes.
[[nodiscard]] std::wstring extended_length_path(const std::filesystem::path& absolute) {
  const std::wstring text = absolute.lexically_normal().make_preferred().native();
  if (text.rfind(L"\\\\?\\", 0) == 0 || text.rfind(L"\\\\.\\", 0) == 0) {
    return text;
  }
  if (text.rfind(L"\\\\", 0) == 0) {
    return L"\\\\?\\UNC\\" + text.substr(2);
  }
  return L"\\\\?\\" + text;
}

/// Owns the temporary sibling of an atomic write. Unless the write published
/// it, the file is removed and the handle closed on every exit path, including
/// the error paths that return early.
class TemporaryFile {
 public:
  TemporaryFile(std::filesystem::path path, HandleGuard handle) : path_(std::move(path)), handle_(std::move(handle)) {}
  ~TemporaryFile() { abandon(); }

  TemporaryFile(TemporaryFile&& other) noexcept = default;
  TemporaryFile& operator=(TemporaryFile&& other) noexcept = default;
  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  [[nodiscard]] HANDLE handle() const noexcept { return handle_.get(); }

  /// Closes the handle. The rename needs the file closed, because the handle
  /// was opened without FILE_SHARE_DELETE.
  void close() noexcept { handle_.reset(); }

  /// Declares the temporary published. Its name now denotes the target.
  void publish() noexcept { published_ = true; }

  void abandon() noexcept {
    handle_.reset();
    if (!published_ && !path_.empty()) {
      DeleteFileW(path_.c_str());
    }
  }

 private:
  std::filesystem::path path_;
  HandleGuard handle_;
  bool published_ = false;
};

}  // namespace

Result<std::filesystem::path> safe_child_path(const std::filesystem::path& root, std::string_view segment) {
  const Result<void> validated = validate_path_segment(segment);
  if (!validated) {
    return validated.error();
  }
  if (root.empty()) {
    return fail(ErrorCode::MalformedRequest, "safe_child_path requires a non-empty root directory");
  }
  const Result<std::wstring> name = to_wide(segment);
  if (!name) {
    return name.error();
  }
  const std::filesystem::path joined = root / std::filesystem::path(name.value());
  const std::size_t units = joined.native().size();
  if (units > kExtendedPathLimit) {
    return fail(ErrorCode::LimitExceeded, "the joined path exceeds the maximum path length")
        .with("path_units", std::to_string(units))
        .with("max_units", std::to_string(kExtendedPathLimit));
  }
  if (units < MAX_PATH) {
    return joined;
  }
  if (!joined.is_absolute()) {
    return fail(ErrorCode::LimitExceeded,
                "a path longer than MAX_PATH must be absolute to use the extended length form")
        .with("path_units", std::to_string(units));
  }
  const std::wstring extended = extended_length_path(joined);
  if (extended.size() > kExtendedPathLimit) {
    return fail(ErrorCode::LimitExceeded, "the extended length path exceeds the maximum path length")
        .with("path_units", std::to_string(extended.size()))
        .with("max_units", std::to_string(kExtendedPathLimit));
  }
  return std::filesystem::path(extended);
}

Result<bool> path_exists(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "path_exists requires a non-empty path");
  }
  const Result<Lookup> found = lookup_object(path, "path_exists");
  if (!found) {
    return found.error();
  }
  return found.value().present;
}

Result<bool> is_directory(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "is_directory requires a non-empty path");
  }
  // GetFileAttributesW reports a directory link or junction as a directory, so
  // this answers "does the path resolve to a directory". is_reparse_point
  // answers the other question, "is the named object itself a link".
  const Result<DWORD> attributes = attributes_of(path, "is_directory");
  if (!attributes) {
    return attributes.error();
  }
  return (attributes.value() & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

Result<bool> is_reparse_point(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "is_reparse_point requires a non-empty path");
  }
  // GetFileAttributesW reports the attributes of the link itself, so the link
  // is classified without being followed.
  const Result<DWORD> attributes = attributes_of(path, "is_reparse_point");
  if (!attributes) {
    return attributes.error();
  }
  return (attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "file_size requires a non-empty path");
  }
  const Result<DWORD> attributes = attributes_of(path, "file_size");
  if (!attributes) {
    return attributes.error();
  }
  if ((attributes.value() & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(ErrorCode::UnsupportedOperation, "file_size requires a regular file, not a directory");
  }
  if ((attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrorCode::ReparsePointRejected, "file_size refuses to operate through a reparse point");
  }
  Result<HandleGuard> handle = open_regular_file(path, FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, "file_size");
  if (!handle) {
    return handle.error();
  }
  return handle_size(handle.value().get(), "file_size");
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "list_directory requires a non-empty path");
  }
  const Result<DWORD> attributes = attributes_of(path, "list_directory");
  if (!attributes) {
    return attributes.error();
  }
  if ((attributes.value() & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return fail(ErrorCode::UnsupportedOperation, "list_directory requires a directory");
  }
  const std::filesystem::path pattern = path / L"*";
  WIN32_FIND_DATAW entry{};
  const HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) {
      // An empty directory and a directory that vanished are different facts,
      // so "empty" is only reported once the directory has been seen again.
      const Result<DWORD> still_there = attributes_of(path, "list_directory");
      if (!still_there) {
        return still_there.error();
      }
      if ((still_there.value() & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return fail(ErrorCode::UnsupportedOperation, "list_directory requires a directory");
      }
      return std::vector<std::string>();
    }
    return win32_failure(ErrorCode::IoFailure, code, "FindFirstFileW");
  }
  const SearchGuard guard(search);
  std::vector<std::string> names;
  for (;;) {
    const std::wstring_view name(entry.cFileName);
    const DWORD entry_attributes = entry.dwFileAttributes;
    const bool regular = (entry_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                         (entry_attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (regular && name != L"." && name != L"..") {
      const Result<std::string> encoded = to_utf8(name);
      if (!encoded) {
        return encoded.error();
      }
      names.push_back(encoded.value());
    }
    if (!FindNextFileW(guard.get(), &entry)) {
      const DWORD code = GetLastError();
      if (code != ERROR_NO_MORE_FILES) {
        return win32_failure(ErrorCode::IoFailure, code, "FindNextFileW");
      }
      break;
    }
  }
  // Sorted so that every caller sees the same order on every machine.
  std::sort(names.begin(), names.end());
  return names;
}

Result<void> remove_file(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "remove_file requires a non-empty path");
  }
  const Result<DWORD> attributes = attributes_of(path, "remove_file");
  if (!attributes) {
    return attributes.error();
  }
  if ((attributes.value() & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(ErrorCode::UnsupportedOperation, "remove_file refuses to remove a directory");
  }
  if ((attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrorCode::ReparsePointRejected, "remove_file refuses to operate through a reparse point");
  }
  if (!DeleteFileW(path.c_str())) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "DeleteFileW");
  }
  return ok();
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "read_file requires a non-empty path");
  }
  const Result<DWORD> attributes = attributes_of(path, "read_file");
  if (!attributes) {
    return attributes.error();
  }
  if ((attributes.value() & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(ErrorCode::UnsupportedOperation, "read_file requires a regular file");
  }
  if ((attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrorCode::ReparsePointRejected, "read_file refuses to operate through a reparse point");
  }
  // Sharing is granted to readers, writers and deleters: this handle never
  // claims exclusive access, so a concurrent writer is possible and the caller
  // has to verify what it read.
  Result<HandleGuard> handle = open_regular_file(path, GENERIC_READ,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, "read_file");
  if (!handle) {
    return handle.error();
  }
  const Result<std::uint64_t> total = handle_size(handle.value().get(), "read_file");
  if (!total) {
    return total.error();
  }
  if (total.value() > max_bytes) {
    return fail(ErrorCode::LimitExceeded, "file is larger than the accepted bound")
        .with("size_bytes", std::to_string(total.value()))
        .with("max_bytes", std::to_string(max_bytes));
  }
  if (total.value() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::LimitExceeded, "file size does not fit the address space")
        .with("size_bytes", std::to_string(total.value()));
  }
  std::vector<std::uint8_t> content(static_cast<std::size_t>(total.value()));
  const Result<void> read = read_exact(handle.value().get(), std::span<std::uint8_t>(content), "read_file");
  if (!read) {
    return read.error();
  }
  return content;
}

Result<void> write_file_atomic(const std::filesystem::path& target, std::span<const std::uint8_t> bytes, bool durable) {
  if (target.empty()) {
    return fail(ErrorCode::MalformedRequest, "write_file_atomic requires a non-empty target path");
  }
  const Result<Lookup> existing = lookup_object(target, "write_file_atomic");
  if (!existing) {
    return existing.error();
  }
  if (existing.value().present) {
    if ((existing.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return fail(ErrorCode::ReparsePointRejected, "write_file_atomic refuses to replace a reparse point");
    }
    if ((existing.value().attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return fail(ErrorCode::UnsupportedOperation, "write_file_atomic cannot replace a directory");
    }
  }
  const Result<std::uint64_t> counter = next_temp_counter();
  if (!counter) {
    return counter.error();
  }
  const std::filesystem::path temporary_path(target.native() + temp_suffix(counter.value()));
  HandleGuard handle(CreateFileW(temporary_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!handle.valid()) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "create temporary file");
  }
  TemporaryFile temporary(temporary_path, std::move(handle));
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(temporary.handle(), &information)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "inspect temporary file");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrorCode::ReparsePointRejected, "the temporary path is a reparse point");
  }
  const Result<void> written = write_all(temporary.handle(), bytes, "write temporary file");
  if (!written) {
    return written.error();
  }
  // The replacement must not be published before the content is on the device,
  // otherwise a power loss leaves a complete looking file with holes.
  if (!FlushFileBuffers(temporary.handle())) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "flush temporary file");
  }
  temporary.close();
  DWORD flags = MOVEFILE_REPLACE_EXISTING;
  if (durable) {
    flags |= MOVEFILE_WRITE_THROUGH;
  }
  // A replace fails with ERROR_ACCESS_DENIED or ERROR_SHARING_VIOLATION while
  // another handle holds the target open, even one that grants
  // FILE_SHARE_DELETE: a reader of the published generation therefore blocks a
  // publish for as long as it holds the file. Every reader in this project opens,
  // reads and closes without holding anything across a publish, so the window is
  // short and bounded, and the replace is retried a bounded number of times
  // rather than failing a commit that had already been flushed to the device.
  // After the bound the operating system error is reported and the previous
  // content stays exactly as it was, which is the safe outcome: the temporary is
  // removed by the guard on every exit path.
  constexpr int kReplaceAttempts = 200;
  DWORD last_error = ERROR_SUCCESS;
  for (int attempt = 0; attempt < kReplaceAttempts; ++attempt) {
    if (MoveFileExW(temporary_path.c_str(), target.c_str(), flags)) {
      temporary.publish();
      return ok();
    }
    last_error = GetLastError();
    if (last_error != ERROR_ACCESS_DENIED && last_error != ERROR_SHARING_VIOLATION &&
        last_error != ERROR_LOCK_VIOLATION) {
      break;
    }
    Sleep(1);
  }
  return win32_failure(ErrorCode::IoFailure, last_error, "replace target file");
}

struct AppendFile::Impl {
  Impl(HANDLE file_handle, std::filesystem::path file_path) : handle(file_handle), path(std::move(file_path)) {}
  ~Impl() { close_handle(); }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void close_handle() noexcept {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
    }
  }

  HANDLE handle = INVALID_HANDLE_VALUE;
  std::filesystem::path path;
};

AppendFile::AppendFile() noexcept = default;
AppendFile::~AppendFile() = default;
AppendFile::AppendFile(AppendFile&& other) noexcept = default;
AppendFile& AppendFile::operator=(AppendFile&& other) noexcept = default;

Result<AppendFile> AppendFile::open(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "AppendFile::open requires a non-empty path");
  }
  const Result<Lookup> existing = lookup_object(path, "AppendFile::open");
  if (!existing) {
    return existing.error();
  }
  if (existing.value().present) {
    if ((existing.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return fail(ErrorCode::ReparsePointRejected, "AppendFile::open refuses to operate through a reparse point");
    }
    if ((existing.value().attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return fail(ErrorCode::UnsupportedOperation, "AppendFile::open requires a regular file");
    }
  }
  // FILE_APPEND_DATA without FILE_WRITE_DATA is what makes every write land at
  // the end of the file, even when another process appends at the same time.
  HandleGuard handle(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!handle.valid()) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "create or open append file");
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle.get(), &information)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "inspect append file");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrorCode::ReparsePointRejected, "AppendFile::open refuses to operate through a reparse point");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(ErrorCode::UnsupportedOperation, "AppendFile::open requires a regular file");
  }
  AppendFile file;
  file.impl_ = std::make_unique<AppendFile::Impl>(handle.release(), path);
  return Result<AppendFile>(std::move(file));
}

Result<void> AppendFile::append(std::span<const std::uint8_t> bytes) {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->handle == INVALID_HANDLE_VALUE) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  if (bytes.empty()) {
    return ok();
  }
  return write_all(impl->handle, bytes, "AppendFile::append");
}

Result<void> AppendFile::flush_to_device() {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->handle == INVALID_HANDLE_VALUE) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  if (!FlushFileBuffers(impl->handle)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "flush append file");
  }
  return ok();
}

Result<std::vector<std::uint8_t>> AppendFile::read_range(std::uint64_t offset, std::size_t count) const {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->handle == INVALID_HANDLE_VALUE) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
    return fail(ErrorCode::LimitExceeded, "the requested offset is outside the file offset range")
        .with("offset", std::to_string(offset));
  }
  if (offset > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(count)) {
    return fail(ErrorCode::LimitExceeded, "the requested range overflows the file offset range")
        .with("offset", std::to_string(offset))
        .with("size_bytes", std::to_string(count));
  }
  // A separate read only handle: the append handle holds no read access and the
  // read must not disturb the append position.
  Result<HandleGuard> handle = open_regular_file(impl->path, GENERIC_READ,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 "AppendFile::read_range");
  if (!handle) {
    return handle.error();
  }
  const Result<std::uint64_t> total = handle_size(handle.value().get(), "AppendFile::read_range");
  if (!total) {
    return total.error();
  }
  const std::uint64_t end = offset + static_cast<std::uint64_t>(count);
  if (total.value() < end) {
    return fail(ErrorCode::IntegrityFailure, "the file is shorter than the requested range")
        .with("file_bytes", std::to_string(total.value()))
        .with("range_end", std::to_string(end));
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle.value().get(), position, nullptr, FILE_BEGIN)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "SetFilePointerEx");
  }
  std::vector<std::uint8_t> content(count);
  const Result<void> read =
      read_exact(handle.value().get(), std::span<std::uint8_t>(content), "AppendFile::read_range");
  if (!read) {
    return read.error();
  }
  return content;
}

Result<std::uint64_t> AppendFile::size() const {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->handle == INVALID_HANDLE_VALUE) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  return handle_size(impl->handle, "AppendFile::size");
}

Result<void> AppendFile::truncate_to(std::uint64_t new_size) {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->handle == INVALID_HANDLE_VALUE) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  const Result<std::uint64_t> current = handle_size(impl->handle, "AppendFile::truncate_to");
  if (!current) {
    return current.error();
  }
  if (new_size > current.value()) {
    // Extending would fabricate zero bytes that were never written.
    return fail(ErrorCode::LimitExceeded, "truncate_to never extends a file")
        .with("current_bytes", std::to_string(current.value()))
        .with("requested_bytes", std::to_string(new_size));
  }
  if (new_size == current.value()) {
    return ok();
  }
  if (new_size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
    return fail(ErrorCode::LimitExceeded, "the requested size is outside the file offset range")
        .with("requested_bytes", std::to_string(new_size));
  }
  // Windows refuses SetEndOfFile on a FILE_APPEND_DATA handle
  // (ERROR_ACCESS_DENIED), and adding FILE_WRITE_DATA would give up the
  // append-at-end-of-file guarantee. A second handle owned by this call
  // performs the truncation; appends on the append handle continue at the new
  // end of the file.
  Result<HandleGuard> handle = open_regular_file(impl->path, GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 "AppendFile::truncate_to");
  if (!handle) {
    return handle.error();
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(new_size);
  if (!SetFilePointerEx(handle.value().get(), position, nullptr, FILE_BEGIN)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "SetFilePointerEx");
  }
  if (!SetEndOfFile(handle.value().get())) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "SetEndOfFile");
  }
  return ok();
}

bool AppendFile::is_open() const noexcept {
  return impl_ != nullptr && impl_->handle != INVALID_HANDLE_VALUE;
}

struct ExclusiveFileLock::Impl {
  Impl() noexcept = default;
  ~Impl() {
    // The byte range lock belongs to the handle. Closing the handle releases
    // it, and the kernel closes it when the process dies, so the lock is
    // released without a destructor, an unlock call or a recovery pass, and
    // UnlockFileEx is deliberately never called.
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
    }
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  HANDLE handle = INVALID_HANDLE_VALUE;
};

ExclusiveFileLock::ExclusiveFileLock() noexcept = default;
ExclusiveFileLock::~ExclusiveFileLock() = default;
ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept = default;
ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept = default;

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "ExclusiveFileLock::acquire requires a non-empty path");
  }
  const Result<Lookup> existing = lookup_object(path, "ExclusiveFileLock::acquire");
  if (!existing) {
    return existing.error();
  }
  if (existing.value().present) {
    if ((existing.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return fail(ErrorCode::ReparsePointRejected,
                  "ExclusiveFileLock::acquire refuses to operate through a reparse point");
    }
    if ((existing.value().attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return fail(ErrorCode::UnsupportedOperation, "ExclusiveFileLock::acquire requires a regular file");
    }
  }
  HandleGuard handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!handle.valid()) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "open lock file");
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle.get(), &information)) {
    return win32_failure(ErrorCode::IoFailure, GetLastError(), "inspect lock file");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrorCode::ReparsePointRejected,
                "ExclusiveFileLock::acquire refuses to operate through a reparse point");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(ErrorCode::UnsupportedOperation, "ExclusiveFileLock::acquire requires a regular file");
  }
  // The whole file, from offset zero to the end of the 64 bit range, taken
  // without waiting: a live holder is an answer, not a reason to block.
  OVERLAPPED overlapped{};
  if (!LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD,
                  &overlapped)) {
    const DWORD code = GetLastError();
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_SHARING_VIOLATION || code == ERROR_IO_PENDING) {
      Error error = fail(ErrorCode::StoreLocked, "the exclusive lock is held by another live process");
      return error.with("win32_error", win32_detail(code));
    }
    return win32_failure(ErrorCode::IoFailure, code, "LockFileEx");
  }
  ExclusiveFileLock lock;
  lock.impl_ = std::make_unique<ExclusiveFileLock::Impl>();
  lock.impl_->handle = handle.release();
  return Result<ExclusiveFileLock>(std::move(lock));
}

bool ExclusiveFileLock::held() const noexcept {
  return impl_ != nullptr && impl_->handle != INVALID_HANDLE_VALUE;
}

std::uint64_t current_process_id() noexcept {
  return static_cast<std::uint64_t>(GetCurrentProcessId());
}

#else  // POSIX

namespace {

#if defined(O_CLOEXEC)
constexpr int kCloseOnExec = O_CLOEXEC;
#else
constexpr int kCloseOnExec = 0;
#endif

#if defined(O_NOFOLLOW)
constexpr int kNoFollow = O_NOFOLLOW;
#else
constexpr int kNoFollow = 0;
#endif

/// Deterministic spelling of a POSIX error. strerror is deliberately not used:
/// its text is localised and is not a stable diagnostic.
[[nodiscard]] std::string errno_detail(int code) {
  std::string detail = std::to_string(code);
  const char* name = nullptr;
  switch (code) {
    case ENOENT:
      name = "ENOENT";
      break;
    case EACCES:
      name = "EACCES";
      break;
    case EPERM:
      name = "EPERM";
      break;
    case EEXIST:
      name = "EEXIST";
      break;
    case EAGAIN:
      name = "EAGAIN";
      break;
    case EISDIR:
      name = "EISDIR";
      break;
    case ENOTDIR:
      name = "ENOTDIR";
      break;
    case ENOSPC:
      name = "ENOSPC";
      break;
    case EROFS:
      name = "EROFS";
      break;
    case EINVAL:
      name = "EINVAL";
      break;
    case ELOOP:
      name = "ELOOP";
      break;
    case ENAMETOOLONG:
      name = "ENAMETOOLONG";
      break;
    case EIO:
      name = "EIO";
      break;
    case EINTR:
      name = "EINTR";
      break;
    default:
      break;
  }
  if (name != nullptr) {
    detail += " (";
    detail += name;
    detail += ")";
  }
  return detail;
}

[[nodiscard]] Error errno_failure(ErrorCode fallback, int code, std::string_view operation) {
  ErrorCode mapped = fallback;
  switch (code) {
    case ENOENT:
    case ENOTDIR:
      mapped = ErrorCode::ObjectNotFound;
      break;
    case EACCES:
    case EPERM:
    case EROFS:
      mapped = ErrorCode::PermissionDenied;
      break;
    case EEXIST:
      mapped = ErrorCode::ObjectExists;
      break;
    case EAGAIN:
      mapped = ErrorCode::StoreLocked;
      break;
    case EISDIR:
      mapped = ErrorCode::UnsupportedOperation;
      break;
    case ELOOP:
      mapped = ErrorCode::ReparsePointRejected;
      break;
    default:
      break;
  }
  Error error = fail(mapped, std::string(operation) + " failed");
  return error.with("errno", errno_detail(code));
}

[[nodiscard]] Error from_error_code(const std::error_code& code, std::string_view operation) {
  ErrorCode mapped = ErrorCode::IoFailure;
  if (code == std::errc::permission_denied) {
    mapped = ErrorCode::PermissionDenied;
  } else if (code == std::errc::no_such_file_or_directory) {
    mapped = ErrorCode::ObjectNotFound;
  } else if (code == std::errc::file_exists) {
    mapped = ErrorCode::ObjectExists;
  } else if (code == std::errc::not_a_directory) {
    mapped = ErrorCode::UnsupportedOperation;
  }
  Error error = fail(mapped, std::string(operation) + " failed");
  return error.with("system_error", "value=" + std::to_string(code.value()) + " category=" + code.category().name());
}

/// Absence of an object and failure of the query that looked for it are
/// different facts, so they are different results. lstat is used so that a link
/// is reported as the object that is there.
struct Lookup {
  struct stat status {};
  bool present = false;
};

[[nodiscard]] Result<Lookup> lookup_object(const std::filesystem::path& path, std::string_view operation) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) == 0) {
    Lookup found;
    found.status = status;
    found.present = true;
    return found;
  }
  const int code = errno;
  if (code == ENOENT || code == ENOTDIR) {
    return Lookup{};
  }
  return errno_failure(ErrorCode::IoFailure, code, operation);
}

[[nodiscard]] Result<struct stat> status_of(const std::filesystem::path& path, std::string_view operation) {
  const Result<Lookup> found = lookup_object(path, operation);
  if (!found) {
    return found.error();
  }
  if (!found.value().present) {
    return fail(ErrorCode::ObjectNotFound, std::string(operation) + ": the object does not exist");
  }
  return found.value().status;
}

/// Closes a descriptor on every exit path.
class FdGuard {
 public:
  FdGuard() noexcept = default;
  explicit FdGuard(int descriptor) noexcept : fd_(descriptor) {}
  ~FdGuard() { reset(); }

  FdGuard(FdGuard&& other) noexcept : fd_(other.release()) {}
  FdGuard& operator=(FdGuard&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.release();
    }
    return *this;
  }
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  int release() noexcept {
    const int released = fd_;
    fd_ = -1;
    return released;
  }

  void reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

/// Opens an existing object and verifies on the descriptor itself that it is a
/// regular file and not a symbolic link.
[[nodiscard]] Result<FdGuard> open_regular_file(const std::filesystem::path& path, int flags,
                                                std::string_view operation) {
  FdGuard descriptor(::open(path.c_str(), flags | kCloseOnExec | kNoFollow));
  if (!descriptor.valid()) {
    const int code = errno;
    if (code == ELOOP) {
      return fail(ErrorCode::ReparsePointRejected,
                  std::string(operation) + " refuses to operate through a symbolic link");
    }
    return errno_failure(ErrorCode::IoFailure, code, operation);
  }
  struct stat status {};
  if (::fstat(descriptor.get(), &status) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, operation);
  }
  if (S_ISLNK(status.st_mode)) {
    return fail(ErrorCode::ReparsePointRejected,
                std::string(operation) + " refuses to operate through a symbolic link");
  }
  if (!S_ISREG(status.st_mode)) {
    return fail(ErrorCode::UnsupportedOperation, std::string(operation) + " requires a regular file");
  }
  return std::move(descriptor);
}

[[nodiscard]] Result<std::uint64_t> descriptor_size(int descriptor, std::string_view operation) {
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, operation);
  }
  if (status.st_size < 0) {
    return fail(ErrorCode::IoFailure, std::string(operation) + ": the file system reported a negative size");
  }
  return static_cast<std::uint64_t>(status.st_size);
}

[[nodiscard]] Result<void> write_all(int descriptor, std::span<const std::uint8_t> bytes, std::string_view operation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_failure(ErrorCode::IoFailure, errno, operation);
    }
    if (written == 0) {
      return fail(ErrorCode::IoFailure, std::string(operation) + " made no progress");
    }
    offset += static_cast<std::size_t>(written);
  }
  return ok();
}

/// Reads exactly the buffer length, or reports IntegrityFailure.
[[nodiscard]] Result<void> read_exact_at(int descriptor, std::uint64_t offset, std::span<std::uint8_t> buffer,
                                         std::string_view operation) {
  std::size_t done = 0;
  while (done < buffer.size()) {
    const ssize_t obtained =
        ::pread(descriptor, buffer.data() + done, buffer.size() - done, static_cast<off_t>(offset + done));
    if (obtained < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_failure(ErrorCode::IoFailure, errno, operation);
    }
    if (obtained == 0) {
      return fail(ErrorCode::IntegrityFailure, std::string(operation) + ": the file ended inside the requested range")
          .with("expected_bytes", std::to_string(buffer.size()))
          .with("read_bytes", std::to_string(done));
    }
    done += static_cast<std::size_t>(obtained);
  }
  return ok();
}

[[nodiscard]] std::string temp_suffix(std::uint64_t counter) {
  return ".tmp-" + std::to_string(current_process_id()) + "-" + std::to_string(counter);
}

/// Makes one directory entry durable by flushing the directory itself. Without
/// this the rename can be lost by a power failure even though the content of
/// the file was flushed.
[[nodiscard]] Result<void> flush_directory(const std::filesystem::path& directory) {
  FdGuard descriptor(::open(directory.c_str(), O_RDONLY | kCloseOnExec));
  if (!descriptor.valid()) {
    return errno_failure(ErrorCode::IoFailure, errno, "open parent directory for flush");
  }
  if (::fsync(descriptor.get()) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "flush parent directory");
  }
  return ok();
}

/// Owns the temporary sibling of an atomic write. Unless the write published
/// it, the file is removed and the descriptor closed on every exit path.
class TemporaryFile {
 public:
  TemporaryFile(std::filesystem::path path, FdGuard descriptor)
      : path_(std::move(path)), descriptor_(std::move(descriptor)) {}
  ~TemporaryFile() { abandon(); }

  TemporaryFile(TemporaryFile&& other) noexcept = default;
  TemporaryFile& operator=(TemporaryFile&& other) noexcept = default;
  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  [[nodiscard]] int descriptor() const noexcept { return descriptor_.get(); }

  /// Closes the descriptor, checking the deferred write error that close can
  /// report. On success the descriptor is closed exactly once.
  [[nodiscard]] Result<void> close() {
    const int descriptor = descriptor_.get();
    if (descriptor < 0) {
      return ok();
    }
    if (::close(descriptor) != 0) {
      descriptor_.release();
      return errno_failure(ErrorCode::IoFailure, errno, "close temporary file");
    }
    descriptor_.release();
    return ok();
  }

  void publish() noexcept { published_ = true; }

  void abandon() noexcept {
    descriptor_.reset();
    if (!published_ && !path_.empty()) {
      ::unlink(path_.c_str());
    }
  }

 private:
  std::filesystem::path path_;
  FdGuard descriptor_;
  bool published_ = false;
};

}  // namespace

Result<std::filesystem::path> safe_child_path(const std::filesystem::path& root, std::string_view segment) {
  const Result<void> validated = validate_path_segment(segment);
  if (!validated) {
    return validated.error();
  }
  if (root.empty()) {
    return fail(ErrorCode::MalformedRequest, "safe_child_path requires a non-empty root directory");
  }
  const std::filesystem::path joined = root / std::filesystem::path(std::string(segment));
#if defined(PATH_MAX)
  const std::size_t bytes = joined.native().size();
  if (bytes >= static_cast<std::size_t>(PATH_MAX)) {
    // There is no prefix form on this platform, so the join is refused rather
    // than attempted and failed later by an unrelated call.
    return fail(ErrorCode::LimitExceeded, "the joined path exceeds the platform path limit")
        .with("path_bytes", std::to_string(bytes))
        .with("max_bytes", std::to_string(static_cast<std::size_t>(PATH_MAX) - 1));
  }
#endif
  return joined;
}

Result<bool> path_exists(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "path_exists requires a non-empty path");
  }
  const Result<Lookup> found = lookup_object(path, "path_exists");
  if (!found) {
    return found.error();
  }
  return found.value().present;
}

Result<bool> is_directory(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "is_directory requires a non-empty path");
  }
  // stat follows a symbolic link, which is the same answer the Windows
  // attributes give for a directory link; is_reparse_point answers the other
  // question, "is the named object itself a link".
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0) {
    const int code = errno;
    if (code == ENOENT || code == ENOTDIR) {
      return fail(ErrorCode::ObjectNotFound, "is_directory: the object does not exist");
    }
    return errno_failure(ErrorCode::IoFailure, code, "stat");
  }
  return S_ISDIR(status.st_mode);
}

Result<bool> is_reparse_point(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "is_reparse_point requires a non-empty path");
  }
  const Result<struct stat> status = status_of(path, "is_reparse_point");
  if (!status) {
    return status.error();
  }
  return S_ISLNK(status.value().st_mode);
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "file_size requires a non-empty path");
  }
  const Result<struct stat> status = status_of(path, "file_size");
  if (!status) {
    return status.error();
  }
  if (S_ISLNK(status.value().st_mode)) {
    return fail(ErrorCode::ReparsePointRejected, "file_size refuses to operate through a symbolic link");
  }
  if (!S_ISREG(status.value().st_mode)) {
    return fail(ErrorCode::UnsupportedOperation, "file_size requires a regular file");
  }
  if (status.value().st_size < 0) {
    return fail(ErrorCode::IoFailure, "file_size: the file system reported a negative size");
  }
  return static_cast<std::uint64_t>(status.value().st_size);
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "list_directory requires a non-empty path");
  }
  // The directory is classified exactly as is_directory classifies it, so the
  // two answers can never disagree. A directory reached through a symbolic link
  // is listed, which is what the Windows attributes report as well.
  const Result<bool> directory = is_directory(path);
  if (!directory) {
    return directory.error();
  }
  if (!directory.value()) {
    return fail(ErrorCode::UnsupportedOperation, "list_directory requires a directory");
  }
  DIR* handle = ::opendir(path.c_str());
  if (handle == nullptr) {
    return errno_failure(ErrorCode::IoFailure, errno, "opendir");
  }
  std::vector<std::string> names;
  for (;;) {
    errno = 0;
    struct dirent* entry = ::readdir(handle);
    if (entry == nullptr) {
      if (errno != 0) {
        const int code = errno;
        ::closedir(handle);
        return errno_failure(ErrorCode::IoFailure, code, "readdir");
      }
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    struct stat entry_status {};
    const std::filesystem::path child = path / name;
    if (::lstat(child.c_str(), &entry_status) != 0) {
      const int code = errno;
      ::closedir(handle);
      return errno_failure(ErrorCode::IoFailure, code, "lstat");
    }
    if (S_ISREG(entry_status.st_mode)) {
      names.push_back(name);
    }
  }
  ::closedir(handle);
  std::sort(names.begin(), names.end());
  return names;
}

Result<void> remove_file(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "remove_file requires a non-empty path");
  }
  const Result<struct stat> status = status_of(path, "remove_file");
  if (!status) {
    return status.error();
  }
  if (S_ISDIR(status.value().st_mode)) {
    return fail(ErrorCode::UnsupportedOperation, "remove_file refuses to remove a directory");
  }
  if (S_ISLNK(status.value().st_mode)) {
    return fail(ErrorCode::ReparsePointRejected, "remove_file refuses to operate through a symbolic link");
  }
  if (::unlink(path.c_str()) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "unlink");
  }
  return ok();
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "read_file requires a non-empty path");
  }
  Result<FdGuard> descriptor = open_regular_file(path, O_RDONLY, "read_file");
  if (!descriptor) {
    return descriptor.error();
  }
  const Result<std::uint64_t> total = descriptor_size(descriptor.value().get(), "read_file");
  if (!total) {
    return total.error();
  }
  if (total.value() > max_bytes) {
    return fail(ErrorCode::LimitExceeded, "file is larger than the accepted bound")
        .with("size_bytes", std::to_string(total.value()))
        .with("max_bytes", std::to_string(max_bytes));
  }
  if (total.value() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::LimitExceeded, "file size does not fit the address space")
        .with("size_bytes", std::to_string(total.value()));
  }
  std::vector<std::uint8_t> content(static_cast<std::size_t>(total.value()));
  const Result<void> read =
      read_exact_at(descriptor.value().get(), 0, std::span<std::uint8_t>(content), "read_file");
  if (!read) {
    return read.error();
  }
  return content;
}

Result<void> write_file_atomic(const std::filesystem::path& target, std::span<const std::uint8_t> bytes, bool durable) {
  if (target.empty()) {
    return fail(ErrorCode::MalformedRequest, "write_file_atomic requires a non-empty target path");
  }
  const Result<Lookup> existing = lookup_object(target, "write_file_atomic");
  if (!existing) {
    return existing.error();
  }
  mode_t mode = 0644;
  if (existing.value().present) {
    if (S_ISLNK(existing.value().status.st_mode)) {
      return fail(ErrorCode::ReparsePointRejected, "write_file_atomic refuses to replace a symbolic link");
    }
    if (!S_ISREG(existing.value().status.st_mode)) {
      return fail(ErrorCode::UnsupportedOperation, "write_file_atomic cannot replace something that is not a regular file");
    }
    // The replacement keeps the permissions of the file it replaces; the
    // temporary itself is created private and never read by anyone else.
    mode = static_cast<mode_t>(existing.value().status.st_mode & 07777);
  }
  const Result<std::uint64_t> counter = next_temp_counter();
  if (!counter) {
    return counter.error();
  }
  const std::filesystem::path temporary_path(target.native() + temp_suffix(counter.value()));
  FdGuard descriptor(::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | kCloseOnExec | kNoFollow, mode));
  if (!descriptor.valid()) {
    const int code = errno;
    if (code == ELOOP) {
      return fail(ErrorCode::ReparsePointRejected, "the temporary path is a symbolic link");
    }
    return errno_failure(ErrorCode::IoFailure, code, "create temporary file");
  }
  TemporaryFile temporary(temporary_path, std::move(descriptor));
  const Result<void> written = write_all(temporary.descriptor(), bytes, "write temporary file");
  if (!written) {
    return written.error();
  }
  if (::fsync(temporary.descriptor()) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "flush temporary file");
  }
  const Result<void> closed = temporary.close();
  if (!closed) {
    return closed.error();
  }
  // rename is atomic within one file system: a reader sees either the whole
  // previous file or the whole new one.
  if (::rename(temporary_path.c_str(), target.c_str()) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "replace target file");
  }
  temporary.publish();
  if (durable) {
    const std::filesystem::path directory = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    const Result<void> flushed = flush_directory(directory);
    if (!flushed) {
      Error error = flushed.error();
      error.message += " (the target was renamed, but the directory entry is not yet durable)";
      return error;
    }
  }
  return ok();
}

struct AppendFile::Impl {
  Impl(int file_descriptor, std::filesystem::path file_path) : descriptor(file_descriptor), path(std::move(file_path)) {}
  ~Impl() { close_descriptor(); }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void close_descriptor() noexcept {
    if (descriptor >= 0) {
      ::close(descriptor);
      descriptor = -1;
    }
  }

  int descriptor = -1;
  std::filesystem::path path;
};

AppendFile::AppendFile() noexcept = default;
AppendFile::~AppendFile() = default;
AppendFile::AppendFile(AppendFile&& other) noexcept = default;
AppendFile& AppendFile::operator=(AppendFile&& other) noexcept = default;

Result<AppendFile> AppendFile::open(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "AppendFile::open requires a non-empty path");
  }
  const Result<Lookup> existing = lookup_object(path, "AppendFile::open");
  if (!existing) {
    return existing.error();
  }
  if (existing.value().present) {
    if (S_ISLNK(existing.value().status.st_mode)) {
      return fail(ErrorCode::ReparsePointRejected, "AppendFile::open refuses to operate through a symbolic link");
    }
    if (!S_ISREG(existing.value().status.st_mode)) {
      return fail(ErrorCode::UnsupportedOperation, "AppendFile::open requires a regular file");
    }
  }
  FdGuard descriptor(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | kCloseOnExec | kNoFollow, 0644));
  if (!descriptor.valid()) {
    const int code = errno;
    if (code == ELOOP) {
      return fail(ErrorCode::ReparsePointRejected, "AppendFile::open refuses to operate through a symbolic link");
    }
    return errno_failure(ErrorCode::IoFailure, code, "open append file");
  }
  struct stat status {};
  if (::fstat(descriptor.get(), &status) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "inspect append file");
  }
  if (!S_ISREG(status.st_mode)) {
    return fail(ErrorCode::UnsupportedOperation, "AppendFile::open requires a regular file");
  }
  AppendFile file;
  file.impl_ = std::make_unique<AppendFile::Impl>(descriptor.release(), path);
  return Result<AppendFile>(std::move(file));
}

Result<void> AppendFile::append(std::span<const std::uint8_t> bytes) {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->descriptor < 0) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  if (bytes.empty()) {
    return ok();
  }
  return write_all(impl->descriptor, bytes, "AppendFile::append");
}

Result<void> AppendFile::flush_to_device() {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->descriptor < 0) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  if (::fsync(impl->descriptor) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "flush append file");
  }
  return ok();
}

Result<std::vector<std::uint8_t>> AppendFile::read_range(std::uint64_t offset, std::size_t count) const {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->descriptor < 0) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  if (offset > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(count)) {
    return fail(ErrorCode::LimitExceeded, "the requested range overflows the file offset range")
        .with("offset", std::to_string(offset))
        .with("size_bytes", std::to_string(count));
  }
  Result<FdGuard> descriptor = open_regular_file(impl->path, O_RDONLY, "AppendFile::read_range");
  if (!descriptor) {
    return descriptor.error();
  }
  const Result<std::uint64_t> total = descriptor_size(descriptor.value().get(), "AppendFile::read_range");
  if (!total) {
    return total.error();
  }
  const std::uint64_t end = offset + static_cast<std::uint64_t>(count);
  if (total.value() < end) {
    return fail(ErrorCode::IntegrityFailure, "the file is shorter than the requested range")
        .with("file_bytes", std::to_string(total.value()))
        .with("range_end", std::to_string(end));
  }
  std::vector<std::uint8_t> content(count);
  const Result<void> read =
      read_exact_at(descriptor.value().get(), offset, std::span<std::uint8_t>(content), "AppendFile::read_range");
  if (!read) {
    return read.error();
  }
  return content;
}

Result<std::uint64_t> AppendFile::size() const {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->descriptor < 0) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  return descriptor_size(impl->descriptor, "AppendFile::size");
}

Result<void> AppendFile::truncate_to(std::uint64_t new_size) {
  const Impl* impl = impl_.get();
  if (impl == nullptr || impl->descriptor < 0) {
    return fail(ErrorCode::StoreClosed, "the append handle is not open");
  }
  const Result<std::uint64_t> current = descriptor_size(impl->descriptor, "AppendFile::truncate_to");
  if (!current) {
    return current.error();
  }
  if (new_size > current.value()) {
    // Extending would fabricate zero bytes that were never written.
    return fail(ErrorCode::LimitExceeded, "truncate_to never extends a file")
        .with("current_bytes", std::to_string(current.value()))
        .with("requested_bytes", std::to_string(new_size));
  }
  if (new_size == current.value()) {
    return ok();
  }
  if (new_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return fail(ErrorCode::LimitExceeded, "the requested size is outside the file offset range")
        .with("requested_bytes", std::to_string(new_size));
  }
  if (::ftruncate(impl->descriptor, static_cast<off_t>(new_size)) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "ftruncate");
  }
  return ok();
}

bool AppendFile::is_open() const noexcept {
  return impl_ != nullptr && impl_->descriptor >= 0;
}

struct ExclusiveFileLock::Impl {
  Impl() noexcept = default;
  ~Impl() {
    // The flock belongs to the open file description. Closing the descriptor
    // releases it, and the kernel closes every descriptor when the process
    // dies, so no destructor, unlink of the lock file or recovery pass is
    // needed for the lock to be released.
    if (descriptor >= 0) {
      ::close(descriptor);
      descriptor = -1;
    }
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  int descriptor = -1;
};

ExclusiveFileLock::ExclusiveFileLock() noexcept = default;
ExclusiveFileLock::~ExclusiveFileLock() = default;
ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept = default;
ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept = default;

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "ExclusiveFileLock::acquire requires a non-empty path");
  }
  const Result<Lookup> existing = lookup_object(path, "ExclusiveFileLock::acquire");
  if (!existing) {
    return existing.error();
  }
  if (existing.value().present) {
    if (S_ISLNK(existing.value().status.st_mode)) {
      return fail(ErrorCode::ReparsePointRejected,
                  "ExclusiveFileLock::acquire refuses to operate through a symbolic link");
    }
    if (!S_ISREG(existing.value().status.st_mode)) {
      return fail(ErrorCode::UnsupportedOperation, "ExclusiveFileLock::acquire requires a regular file");
    }
  }
  FdGuard descriptor(::open(path.c_str(), O_RDWR | O_CREAT | kCloseOnExec | kNoFollow, 0644));
  if (!descriptor.valid()) {
    const int code = errno;
    if (code == ELOOP) {
      return fail(ErrorCode::ReparsePointRejected,
                  "ExclusiveFileLock::acquire refuses to operate through a symbolic link");
    }
    return errno_failure(ErrorCode::IoFailure, code, "open lock file");
  }
  struct stat status {};
  if (::fstat(descriptor.get(), &status) != 0) {
    return errno_failure(ErrorCode::IoFailure, errno, "inspect lock file");
  }
  if (!S_ISREG(status.st_mode)) {
    return fail(ErrorCode::UnsupportedOperation, "ExclusiveFileLock::acquire requires a regular file");
  }
  if (::flock(descriptor.get(), LOCK_EX | LOCK_NB) != 0) {
    const int code = errno;
    if (code == EAGAIN || code == EWOULDBLOCK) {
      Error error = fail(ErrorCode::StoreLocked, "the exclusive lock is held by another live process");
      return error.with("errno", errno_detail(code));
    }
    return errno_failure(ErrorCode::IoFailure, code, "flock");
  }
  ExclusiveFileLock lock;
  lock.impl_ = std::make_unique<ExclusiveFileLock::Impl>();
  lock.impl_->descriptor = descriptor.release();
  return Result<ExclusiveFileLock>(std::move(lock));
}

bool ExclusiveFileLock::held() const noexcept {
  return impl_ != nullptr && impl_->descriptor >= 0;
}

std::uint64_t current_process_id() noexcept {
  return static_cast<std::uint64_t>(::getpid());
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Definitions that do not depend on the platform.
// ---------------------------------------------------------------------------

Result<void> validate_path_segment(std::string_view segment) {
  if (segment.empty()) {
    return fail(ErrorCode::MalformedRequest, "path segment is empty");
  }
  if (segment == "." || segment == "..") {
    return fail(ErrorCode::PathTraversal, "path segment is a relative directory reference")
        .with("segment", std::string(segment));
  }
  for (std::size_t index = 0; index < segment.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(segment[index]);
    if (byte == static_cast<unsigned char>('/') || byte == static_cast<unsigned char>('\\')) {
      return fail(ErrorCode::PathTraversal, "path segment contains a directory separator");
    }
    if (byte == static_cast<unsigned char>(':')) {
      return fail(ErrorCode::AlternateDataStream, "path segment contains an alternate data stream colon");
    }
    if (byte < 0x20) {
      return fail(ErrorCode::MalformedRequest, "path segment contains a control character or NUL")
          .with("byte", std::to_string(static_cast<unsigned int>(byte)));
    }
  }
  if (segment.back() == '.' || segment.back() == ' ') {
    return fail(ErrorCode::MalformedRequest, "path segment ends with a dot or a space");
  }
  if (is_reserved_device_name(segment)) {
    return fail(ErrorCode::MalformedRequest, "path segment is a reserved device name")
        .with("segment", std::string(segment));
  }
  const Result<std::size_t> units = utf16_unit_count(segment);
  if (!units) {
    return units.error();
  }
  if (units.value() > limits::kMaxPathUnits) {
    // A bound from limits.hpp is reported as LimitExceeded, never satisfied by
    // dropping the excess.
    return fail(ErrorCode::LimitExceeded, "path segment is longer than the accepted bound")
        .with("path_units", std::to_string(units.value()))
        .with("max_units", std::to_string(limits::kMaxPathUnits));
  }
  return ok();
}

Result<void> ensure_directory(const std::filesystem::path& path) {
  if (path.empty()) {
    return fail(ErrorCode::MalformedRequest, "ensure_directory requires a non-empty path");
  }
  std::error_code code;
  // create_directories reports success when the directory already exists, and
  // reports an error when the path exists as something else. Nothing is
  // repaired here.
  std::filesystem::create_directories(path, code);
  if (code) {
    return from_error_code(code, "create_directories");
  }
  return ok();
}

}  // namespace hardware_lifecycle::detail
