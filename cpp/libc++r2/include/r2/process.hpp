#ifndef _R2CXX_PROCESS_HPP_
#define _R2CXX_PROCESS_HPP_

/*
 *  process.hpp — the program itself: arguments, exit, other tasks.
 */

#include "optional.hpp"
#include "string.hpp"
#include "syscall.hpp"
#include "vector.hpp"

namespace r2 {

/*  Command line, as the kernel handed it over.  arg(0) is the program name.  */
int arg_count() noexcept;
string_view arg(int index) noexcept;

/*
 *  Ends the process: flushes the console, runs the functions registered with
 *  at_exit() and the destructors of objects with static storage duration, then
 *  makes the exit syscall.
 */
[[noreturn]] void exit(int code = 0);

/*  Ends the process without running any of that.  For use when the program
 *  state is already suspect.  */
[[noreturn]] void quick_exit(int code);

/*
 *  Prints a message and exits with code 1.  This is what the runtime calls
 *  when it gives up --- a pure virtual call, a failed assertion, an attempt to
 *  throw with exceptions disabled.
 */
[[noreturn]] void panic(string_view message);

/*  Registers a function to run at exit.  Up to 32; returns false past that.  */
bool at_exit(void (*fn)());

/*  Sysinfo (syscall 0x01): host name, user, version, path, uptime, IP.  */
optional<SysInfo> sysinfo();
bool set_sysinfo(const SysInfo &info);

/*  The scheduler's task table (syscall 0x2f), at most 10 entries.  */
vector<TaskInfo> tasks();

enum class TaskMode : uint8_t { Kernel = 0, User = 1 };
enum class TaskStatus : uint8_t {
    Ready = 0,
    Running = 1,
    Idle = 2,
    Blocked = 3,
    Crashed = 4,
    Dead = 5,
};

string_view task_status_name(uint8_t status);

/*
 *  Launches another ELF in the background (syscall 0x2a).  `args` is a
 *  space-delimited string whose first token becomes argv[0]; when it is empty
 *  the kernel uses the program name.  Returns the new pid.
 */
optional<uint8_t> spawn(string_view path, string_view args = string_view());

} // namespace r2

/*
 *  R2_ASSERT — panics with file and line when the condition does not hold.
 *  Compiled out entirely when R2_NDEBUG is defined.
 */
#ifdef R2_NDEBUG
#define R2_ASSERT(cond) ((void)0)
#else
#define R2_ASSERT(cond)                                                                            \
    ((cond) ? (void)0 : ::r2::panic(::r2::string_view(__FILE__ ":" R2_STRINGIFY(__LINE__) ": " #cond)))
#define R2_STRINGIFY_(x) #x
#define R2_STRINGIFY(x) R2_STRINGIFY_(x)
#endif

#endif
