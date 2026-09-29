// Hardware Lifecycle - real child processes for the multiprocess proofs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "process.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <cstdlib>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace hl_test {
namespace {

#if defined(_WIN32)
[[nodiscard]] std::string quote_argument(const std::string& argument) {
  if (argument.find_first_of(" \t\"") == std::string::npos) {
    return argument;
  }
  std::string quoted = "\"";
  std::size_t backslashes = 0;
  for (const char character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted.push_back('"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, '\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, '\\');
  quoted.push_back('"');
  return quoted;
}
#endif

}  // namespace

ChildProcess::~ChildProcess() {
  if (started_ && !finished_) {
    terminate(0);
  }
#if defined(_WIN32)
  if (read_end_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(read_end_));
    read_end_ = nullptr;
  }
#else
  if (read_fd_ >= 0) {
    close(read_fd_);
    read_fd_ = -1;
  }
#endif
}

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& arguments,
                         std::string& error) {
  if (started_) {
    error = "the child was already started";
    return false;
  }
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (CreatePipe(&read_end, &write_end, &attributes, 0) == FALSE) {
    error = "CreatePipe failed";
    return false;
  }
  if (SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0) == FALSE) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    error = "SetHandleInformation failed";
    return false;
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  std::string command_line = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    command_line += quote_argument(argument);
  }
  std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back('\0');

  PROCESS_INFORMATION information{};
  const BOOL created = CreateProcessA(nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                      nullptr, &startup, &information);
  CloseHandle(write_end);
  if (created == FALSE) {
    const DWORD code = GetLastError();
    CloseHandle(read_end);
    error = "CreateProcess failed with " + std::to_string(code);
    return false;
  }
  process_ = information.hProcess;
  thread_ = information.hThread;
  read_end_ = read_end;
  pid_ = information.dwProcessId;
  started_ = true;
  return true;
#else
  int pipe_descriptors[2] = {-1, -1};
  if (pipe(pipe_descriptors) != 0) {
    error = "pipe failed";
    return false;
  }
  const pid_t child = fork();
  if (child < 0) {
    close(pipe_descriptors[0]);
    close(pipe_descriptors[1]);
    error = "fork failed";
    return false;
  }
  if (child == 0) {
    dup2(pipe_descriptors[1], STDOUT_FILENO);
    dup2(pipe_descriptors[1], STDERR_FILENO);
    close(pipe_descriptors[0]);
    close(pipe_descriptors[1]);
    std::vector<char*> argv;
    std::string program = executable;
    argv.push_back(program.data());
    std::vector<std::string> owned = arguments;
    for (std::string& argument : owned) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  close(pipe_descriptors[1]);
  child_pid_ = static_cast<int>(child);
  read_fd_ = pipe_descriptors[0];
  pid_ = static_cast<std::uint64_t>(child);
  started_ = true;
  return true;
#endif
}

bool ChildProcess::read_line(std::string& line) {
  line.clear();
  if (!started_) {
    return false;
  }
  for (;;) {
    char character = 0;
#if defined(_WIN32)
    DWORD read = 0;
    if (ReadFile(static_cast<HANDLE>(read_end_), &character, 1, &read, nullptr) == FALSE || read == 0) {
      return !line.empty();
    }
#else
    const ssize_t read = ::read(read_fd_, &character, 1);
    if (read <= 0) {
      return !line.empty();
    }
#endif
    if (character == '\n') {
      return true;
    }
    if (character != '\r') {
      line.push_back(character);
    }
  }
}

std::vector<std::string> ChildProcess::read_lines_until_close() {
  std::vector<std::string> lines;
  std::string line;
  while (read_line(line)) {
    lines.push_back(line);
  }
  return lines;
}

int ChildProcess::wait() {
  if (!started_) {
    return -1;
  }
  if (finished_) {
    return exit_code_;
  }
#if defined(_WIN32)
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
#else
  int status = 0;
  waitpid(child_pid_, &status, 0);
  if (WIFEXITED(status)) {
    exit_code_ = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    exit_code_ = 128 + WTERMSIG(status);
  } else {
    exit_code_ = -1;
  }
#endif
  finished_ = true;
  return exit_code_;
}

void ChildProcess::terminate(int exit_code) {
  if (!started_ || finished_) {
    return;
  }
#if defined(_WIN32)
  TerminateProcess(static_cast<HANDLE>(process_), static_cast<UINT>(exit_code));
  WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  exit_code_ = static_cast<int>(code);
  finished_ = true;
  if (thread_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
  }
  if (process_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
#else
  (void)exit_code;
  kill(child_pid_, SIGKILL);
  int status = 0;
  waitpid(child_pid_, &status, 0);
  exit_code_ = 1;
  finished_ = true;
#endif
}

}  // namespace hl_test
