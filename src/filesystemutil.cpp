// SPDX-License-Identifier: MIT
#include "filesystemutil.hpp"

#include <algorithm>
#include <cassert>

namespace makebelieve {

bool FileSystemUtil::are_disjoint(std::filesystem::path a,
                                  std::filesystem::path b) {
  assert(a.is_absolute());
  assert(b.is_absolute());
  a = std::filesystem::weakly_canonical(a);
  b = std::filesystem::weakly_canonical(b);
  const auto [a_it, b_it] = std::ranges::mismatch(a, b);
  // If either range is exhausted it is a prefix of the other - an ancestor of,
  // or equal to, it - so the two overlap. They are disjoint only when both
  // still have a component left, i.e. they diverged before either ran out.
  return a_it != a.end() && b_it != b.end();
}

}  // namespace makebelieve
