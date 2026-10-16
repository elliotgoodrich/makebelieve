// SPDX-License-Identifier: MIT
#include "scopeguard.hpp"

#include <gtest/gtest.h>

namespace makebelieve {
namespace {

template <class F>
concept Guardable = requires { typename ScopeGuard<F>; };

static_assert(Guardable<void (*)()>);
static_assert(!Guardable<int>);
static_assert(!Guardable<void (*)(int)>);

TEST(ScopeGuardTest, RunsOnExitWhenDestroyed) {
  int calls = 0;
  {
    const ScopeGuard guard([&] { ++calls; });
    EXPECT_EQ(calls, 0);
  }
  EXPECT_EQ(calls, 1);
}

TEST(ScopeGuardTest, DismissSuppressesOnExit) {
  int calls = 0;
  {
    ScopeGuard guard([&] { ++calls; });
    guard.dismiss();
  }
  EXPECT_EQ(calls, 0);
}

}  // namespace
}  // namespace makebelieve
