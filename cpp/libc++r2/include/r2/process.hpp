#ifndef _R2CXX_PROCESS_HPP_
#define _R2CXX_PROCESS_HPP_

/*
 *  process.hpp — the program itself: arguments, exit, other tasks.
 */

#include "optional.hpp"
#include "panic.hpp"
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

#endif
