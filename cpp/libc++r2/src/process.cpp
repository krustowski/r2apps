/*
 *  process.cpp — sysinfo, the task table, and launching other programs.
 */

#include "r2/libc.hpp"
#include "r2/process.hpp"

namespace r2 {

namespace {

/*
 *  Copies a path or name into a NUL-terminated buffer on the stack.  Every
 *  string that crosses the ABI has to be NUL-terminated and inside the range
 *  the kernel accepts, and a string_view is neither by construction.
 */
template <size_t N> bool to_c_buffer(string_view text, char (&buffer)[N]) {
    if (text.size() >= N)
        return false;
    memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return true;
}

} // namespace

optional<SysInfo> sysinfo() {
    SysInfo info;
    memset(&info, 0, sizeof(info));

    if (raw_syscall(Sys::SysInfo, 0x01, (int64_t)&info) != 0)
        return nullopt;

    return info;
}

bool set_sysinfo(const SysInfo &info) {
    return raw_syscall(Sys::SysInfo, 0x02, (int64_t)&info) == 0;
}

bool set_user(string_view name) {
    SysInfo info;
    memset(&info, 0, sizeof(info));
    if (name.empty() || name.size() >= sizeof(info.system_user))
        return false;
    memcpy(info.system_user, name.data(), name.size());
    return raw_syscall(Sys::SysInfo, 0x03, (int64_t)&info) == 0;
}

vector<TaskInfo> tasks() {
    constexpr uint8_t MAX_TASKS = 10; /*  the kernel's own limit  */
    TaskInfo buffer[MAX_TASKS];
    memset(buffer, 0, sizeof(buffer));

    int64_t count = raw_syscall(Sys::ListTasks, (int64_t)buffer, MAX_TASKS);

    vector<TaskInfo> result;
    if (count <= 0 || count > MAX_TASKS)
        return result;

    if (!result.reserve((size_t)count))
        return result;

    for (int64_t i = 0; i < count; i++)
        (void)result.push_back(buffer[i]);

    return result;
}

string_view task_status_name(uint8_t status) {
    switch (status) {
    case 0:
        return string_view("ready");
    case 1:
        return string_view("running");
    case 2:
        return string_view("idle");
    case 3:
        return string_view("blocked");
    case 4:
        return string_view("crashed");
    case 5:
        return string_view("dead");
    default:
        return string_view("unknown");
    }
}

optional<uint8_t> spawn(string_view path, string_view args) {
    char path_buffer[64];
    char args_buffer[128];

    if (!to_c_buffer(path, path_buffer))
        return nullopt;
    if (!to_c_buffer(args, args_buffer))
        return nullopt;

    int64_t result = raw_syscall(Sys::RunElf, (int64_t)path_buffer,
                                 args.empty() ? 0 : (int64_t)args_buffer);
    if (result == 0)
        return nullopt;

    return (uint8_t)result;
}

bool kill(uint8_t id) {
    return raw_syscall(Sys::KillTask, (int64_t)id, 0) == 0;
}

optional<string> command_line(uint8_t id) {
    constexpr int64_t BUSY = 0xfa; /*  the scheduler was locked: ask again  */
    char line[128];
    for (int tries = 0; tries < 8; tries++) {
        int64_t n = raw_syscall(Sys::Cmdline, (int64_t)id, (int64_t)line);
        if (n == BUSY)
            continue;
        if (n < 0 || n > (int64_t)sizeof(line))
            return nullopt;
        return string(line, (size_t)n);
    }
    return nullopt;
}

bool reboot() noexcept {
    raw_syscall(Sys::Power, 0x01, 0);
    return false; // still here: a kernel without the call
}

bool power_off() noexcept {
    raw_syscall(Sys::Power, 0x02, 0);
    return false;
}

optional<MemInfo> meminfo() {
    MemInfo info;
    memset(&info, 0, sizeof(info));
    if (raw_syscall(Sys::MemInfo, (int64_t)&info, 0) != 0 || info.version < 1)
        return nullopt;
    return info;
}

} // namespace r2
