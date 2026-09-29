#pragma once

// Hardware Lifecycle - real child processes for the multiprocess proofs.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Claims about a second operating system process, about an operating system
// level lock or about crash consistency are proven with real processes, never
// with threads standing in for processes and never with a simulation.
//
// The helper deliberately offers no timed read and no timed wait: this project
// does not use timeouts. A child that never answers is a defect to diagnose, and
// the harness is designed so that such a child blocks visibly instead of being
// silently abandoned.

#include <cstdint>
#include <string>
#include <vector>

namespace hl_test {

/// A child process whose standard output is captured through a pipe.
class ChildProcess {
 public:
  ChildProcess() noexcept = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Starts the program. The command line is built from the arguments with
  /// Windows quoting rules; an argument containing a space or a quote is
  /// quoted. Returns false and fills error on failure.
  [[nodiscard]] bool start(const std::string& executable, const std::vector<std::string>& arguments,
                           std::string& error);

  /// Reads one line of the child's standard output, blocking until a newline
  /// arrives or the child closes its output. Returns false at end of output.
  [[nodiscard]] bool read_line(std::string& line);

  /// Reads until the child closes its output. This is how a crashing child is
  /// observed: its output ends when its process ends.
  [[nodiscard]] std::vector<std::string> read_lines_until_close();

  /// Blocks until the child exits and returns its exit code. On Windows a
  /// process killed by TerminateProcess reports the code it was killed with.
  [[nodiscard]] int wait();

  /// Terminates the child with the given code and waits for it. Used to prove
  /// that the operating system releases a file lock when a process dies.
  void terminate(int exit_code);

  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

 private:
#if defined(_WIN32)
  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* read_end_ = nullptr;
#else
  int child_pid_ = -1;
  int read_fd_ = -1;
#endif
  std::uint64_t pid_ = 0;
  bool started_ = false;
  bool finished_ = false;
  int exit_code_ = -1;
};

}  // namespace hl_test
