#pragma once
#include "types.hpp"
namespace r2 {
enum class Sys { SysInfo = 0x01, SendPacket = 0x34, ReceivePort = 0x35, NetRegister = 0x37 };
struct SysInfo { uint8_t ip_addr[4]; };
int64_t raw_syscall(Sys number, int64_t arg1 = 0, int64_t arg2 = 0, int64_t arg3 = 0) noexcept;
}
