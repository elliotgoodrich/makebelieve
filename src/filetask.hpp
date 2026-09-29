// SPDX-License-Identifier: MIT
#pragma once

#include "directorytree.hpp"

#include <exec/task.hpp>

namespace makebelieve {
/// Converts @a task into a file sender, establishing its start scheduler
/// before type erasure. The inline scheduler leaves continuations on their
/// completion thread; stop tokens propagate from the receiver. The task must
/// arrange any executor handoff needed by its own operations.
/// This sets the wrapped task's scheduler, not that of a coroutine awaiting
/// the returned sender; the caller retains its own completion affinity.
template <class T>
FileSender<T> file_task(exec::task<std::expected<T, std::error_code>> task) {
  return stdexec::write_env(
      std::move(task),
      stdexec::prop{stdexec::get_start_scheduler, stdexec::inline_scheduler{}});
}
}  // namespace makebelieve
