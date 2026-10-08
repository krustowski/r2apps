#pragma once
#include <r2/types.hpp>
// Host tests retry operations without entering the r2 syscall ABI.
namespace r2 { void sleep(uint64_t ms) noexcept; }
