#ifndef _R2CXX_SOURCE_LOCATION_HPP_
#define _R2CXX_SOURCE_LOCATION_HPP_

/*
 *  source_location.hpp — where a call came from, without a macro at the call
 *  site.
 *
 *  Built on __builtin_FILE/__builtin_LINE/__builtin_FUNCTION evaluated as
 *  default arguments, which is how the standard version works too.  A function
 *  that takes `source_location loc = source_location::current()` is told the
 *  caller's position, so panic() and the assertions can report it without
 *  every caller passing __FILE__ along.
 */

#include "types.hpp"

namespace r2 {

class source_location {
  public:
    static constexpr source_location current(const char *file = __builtin_FILE(),
                                             const char *function = __builtin_FUNCTION(),
                                             uint32_t line = __builtin_LINE()) noexcept {
        source_location loc;
        loc.file_ = file;
        loc.function_ = function;
        loc.line_ = line;
        return loc;
    }

    constexpr source_location() noexcept : file_("unknown"), function_("unknown"), line_(0) {}

    constexpr const char *file_name() const noexcept { return file_; }
    constexpr const char *function_name() const noexcept { return function_; }
    constexpr uint32_t line() const noexcept { return line_; }

  private:
    const char *file_;
    const char *function_;
    uint32_t line_;
};

} // namespace r2

#endif
