// SPDX-License-Identifier: MIT
#pragma once

namespace makebelieve {

#ifdef _WIN32
/// A `HANDLE`, spelled without <windows.h>.
using NativeHandle = void*;
#else
/// A file descriptor.
using NativeHandle = int;
#endif

}  // namespace makebelieve
