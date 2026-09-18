#ifndef _R2CXX_HPP_
#define _R2CXX_HPP_

/*
 *  r2.hpp — libc++r2, the C++ runtime and standard library for the r2 kernel.
 *
 *  One include for everything.  A program that wants only part of it can
 *  include the individual headers instead; they are all self-contained.
 *
 *      #include <r2.hpp>
 *
 *      int main(int argc, char **argv) {
 *          r2::println("hello from C++ on r2");
 *          return 0;
 *      }
 *
 *  Build with cpp/libc++r2/Makefile.tmpl; see README.md.
 */

#include "r2/types.hpp"

#include "r2/algorithm.hpp"
#include "r2/array.hpp"
#include "r2/function.hpp"
#include "r2/initializer_list.hpp"
#include "r2/memory.hpp"
#include "r2/new.hpp"
#include "r2/optional.hpp"
#include "r2/span.hpp"
#include "r2/string.hpp"
#include "r2/string_view.hpp"
#include "r2/type_traits.hpp"
#include "r2/utility.hpp"
#include "r2/vector.hpp"

#include "r2/audio.hpp"
#include "r2/fs.hpp"
#include "r2/gfx.hpp"
#include "r2/heap.hpp"
#include "r2/input.hpp"
#include "r2/io.hpp"
#include "r2/libc.hpp"
#include "r2/math.hpp"
#include "r2/net.hpp"
#include "r2/process.hpp"
#include "r2/syscall.hpp"
#include "r2/time.hpp"

#endif
