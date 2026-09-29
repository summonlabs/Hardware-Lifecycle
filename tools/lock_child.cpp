// Hardware Lifecycle - the writer lock child.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Holds the store's writer lock until the operating system takes the process
// away. The program opens the store as a writer, announces itself and then
// sleeps forever; it never closes the store, never returns from main and never
// releases the lock on its own, so the parent proves that the lock is released
// by process termination rather than by cooperation.
//
//   lock_child --store <dir>
//
// Exit codes: 0 unreachable (the process is terminated), 1 usage error, 2 the
// store could not be opened as a writer.

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <windows.h>

#include "hardware_lifecycle/hardware_lifecycle.hpp"

namespace hw = hardware_lifecycle;

int main(int argc, char** argv) {
  std::string store;
  bool store_given = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view token = argv[index];
    if (token == "--store") {
      if (index + 1 >= argc) {
        std::cout << "lock_child: option --store requires a value\n";
        std::cout.flush();
        return 1;
      }
      if (store_given) {
        std::cout << "lock_child: option --store was given more than once\n";
        std::cout.flush();
        return 1;
      }
      ++index;
      store = argv[index];
      store_given = true;
    } else {
      std::cout << "lock_child: unknown option " << token << '\n';
      std::cout.flush();
      return 1;
    }
  }
  if (!store_given) {
    std::cout << "lock_child: usage: lock_child --store <dir>\n";
    std::cout.flush();
    return 1;
  }

  hw::OpenOptions options;
  options.root = std::filesystem::path(store);
  options.read_only = false;
  options.create_if_missing = true;
  hw::Result<hw::Runtime> runtime = hw::Runtime::open(options);
  if (!runtime.has_value()) {
    std::cout << "lock_child: " << hw::describe(runtime.error()) << '\n';
    std::cout.flush();
    return 2;
  }

  // The lock is held for as long as this process lives: the loop below never
  // returns, so the runtime is never destroyed and nothing is released here.
  std::cout << "ready " << GetCurrentProcessId() << '\n';
  std::cout.flush();
  for (;;) {
    Sleep(1000);
  }
}
