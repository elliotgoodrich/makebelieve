// SPDX-License-Identifier: MIT
#pragma once

#include "processutil.hpp"

#include <optional>
#include <system_error>

namespace makebelieve::detail {

/// For ProcessUtil's implementations alone: the failure `ProcessUtilTestUtil`
/// has due next at @a failure, if one is, which this consumes.
std::optional<std::error_code> take_forced_failure(
    ProcessUtilTestUtil::Failure failure);

}  // namespace makebelieve::detail
