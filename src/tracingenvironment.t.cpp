// SPDX-License-Identifier: MIT
#include "tracingenvironment.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using makebelieve::TracingEnvironment;

// The entries of @a block, in order.
std::vector<std::wstring> entries(const std::wstring& block) {
  return TracingEnvironment::entries_of(block.c_str());
}

TEST(TracingEnvironment, ReplacesTracingVariablesWhateverTheirCase) {
  const std::wstring block = TracingEnvironment::build(
      {L"makebelieve_trace_root=C:/unrelated",
       L"MakeBelieve_Trace_Log=C:/other.log", L"PATH=C:/bin"},
      {{L"MAKEBELIEVE_TRACE_ROOT", L"C:/work"},
       {L"MAKEBELIEVE_TRACE_LOG", L"C:/trace.log"}});
  EXPECT_EQ(entries(block),
            (std::vector<std::wstring>{L"MAKEBELIEVE_TRACE_LOG=C:/trace.log",
                                       L"MAKEBELIEVE_TRACE_ROOT=C:/work",
                                       L"PATH=C:/bin"}));
}

// CreateProcess wants a block sorted by name without regard to case - as
// upper case, so `_` sorts after letters - and the per-drive current
// directories, whose names begin with `=`, come first.
TEST(TracingEnvironment, SortsByNameIgnoringCase) {
  const std::wstring block = TracingEnvironment::build(
      {L"zeta=1", L"Alpha=2", L"=C:=C:/work", L"beta=3", L"B_=4"}, {});
  EXPECT_EQ(entries(block),
            (std::vector<std::wstring>{L"=C:=C:/work", L"Alpha=2", L"beta=3",
                                       L"B_=4", L"zeta=1"}));
}

TEST(TracingEnvironment, ReadsAnAnsiBlock) {
  const char block[] = "A=1\0B=two\0";
  EXPECT_EQ(TracingEnvironment::entries_of(block),
            (std::vector<std::wstring>{L"A=1", L"B=two"}));
}

}  // namespace
