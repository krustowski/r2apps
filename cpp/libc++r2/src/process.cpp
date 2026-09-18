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

} // namespace r2
