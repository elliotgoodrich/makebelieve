// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

// What `makebelieve --help` and friends print. The binary is often all a user
// has - an agent in a project that uses makebelieve never sees this repo's
// docs - so it describes itself, and the manifest format, in full.
namespace makebelieve::help {

// `makebelieve --help`: what makebelieve is, and every command.
[[nodiscard]] std::string_view overview();

// `makebelieve mount --help`.
[[nodiscard]] std::string_view mount();

// `makebelieve unmount --help`.
[[nodiscard]] std::string_view unmount();

// `makebelieve help manifest`: a condensed reference for build.makebelieve,
// kept in step with DOCS.md by the makebelieve_help test.
[[nodiscard]] std::string_view manifest();

// `makebelieve --version`: the project version, and the commit it was
// configured from when known, as one line without a newline.
[[nodiscard]] std::string_view version();

}  // namespace makebelieve::help
