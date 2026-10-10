//
// netmux — one process, two network stacks, one frame queue.
//
// The kernel queues a process's network frames in one queue (syscall 0x35).
// Memento runs two TCP/IP stacks: the web browser's (web/net_r2.cpp) and
// c/libcr2's, which the Chat and IRC windows use.  If both read the queue,
// each takes frames that were the other's and throws them away --- a page
// that loads while IRC is connected loses its segments, and IRC its lines.
//
// So neither reads it.  Both ask here: the queue is read in one place, each
// frame goes to the stack that owns its destination port (the browser claims
// its ports with netmux_claim()), and a frame for the stack that is not asking
// just now waits for it in a small queue of its own.  ARP goes to both, since
// both keep a cache of addresses; ping to one, so it is answered once.
//
// A third client, the Network window's tools (nsk.cpp: the host scan after
// c/nsk, ping and traceroute), is no stack: it takes the replies to its own
// probes --- echo replies with one of its ICMP ids, the errors routers send
// back about such a probe, TCP to the port it binds --- and a copy of every
// ARP frame while it is asking, and nothing else.
//
// Every frame this process sends goes out through netmux_send() as well, so
// that what comes in and what goes out can be counted in one place: Memento's
// own traffic. The Network window reads system NIC totals through syscall
// 0x45 and uses these local counters as a fallback on older kernels.
//
#pragma once

#include <r2/types.hpp>

enum NetmuxClient
{
    NETMUX_WEB = 0,
    NETMUX_CR2 = 1, // c/libcr2: Chat, IRC
    NETMUX_NSK = 2, // the Network window's scan, ping and traceroute
    NETMUX_CLIENTS = 3,
};

//  The ICMP echo ids of the Network window's probes: echo replies carrying
//  one, and time-exceeded or unreachable errors quoting a probe that carried
//  one, are NETMUX_NSK's.  The scan's is 'NS', as c/nsk has it.
#define NETMUX_NSK_ICMP_ID 0x4e53
#define NETMUX_PING_ICMP_ID 0x5047  // 'PG'
#define NETMUX_TRACE_ICMP_ID 0x5452 // 'TR'

//  One frame for `who`: copied into `buf` (at most `cap` bytes), its length
//  returned, or 0 when there is none just now.
extern "C" int64_t netmux_pull(int who, uint8_t *buf, uint32_t cap);

//  TCP and UDP destination ports lo..hi (inclusive) are `who`'s.  Everything
//  not claimed is c/libcr2's.
extern "C" void netmux_claim(int who, uint16_t lo, uint16_t hi);

// Stop listening and discard queued frames when a tool session ends.
extern "C" void netmux_forget(int who);

//  Frames each stack got, and frames dropped because its queue here was full
//  or it had stopped asking; and the process's traffic as a whole, every
//  frame taken from the kernel's queue and every frame sent.
struct NetmuxStats
{
    uint64_t frames[NETMUX_CLIENTS];
    uint64_t dropped[NETMUX_CLIENTS];
    uint64_t rx_frames, rx_bytes;
    uint64_t tx_frames, tx_bytes;
};
extern "C" NetmuxStats netmux_stats();

//  Sends one Ethernet frame (syscall 0x34, kind 0x04) and counts it.
extern "C" bool netmux_send(const uint8_t *frame, uint32_t len);

//  The TCP ports this process has bound (syscall 0x37), and what for: "web",
//  "scan", "chat". Syscall 0x46 reports every port's PID and process name;
//  these notes add Memento's in-process use. netmux_port_use() is null for
//  a port not noted here.
extern "C" void netmux_port_bound(uint16_t port, const char *use);
extern "C" void netmux_port_released(uint16_t port);
extern "C" const char *netmux_port_use(uint16_t port);

//  The global Ethernet driver registration (syscall 0x37) is the process's,
//  not a stack's: the kernel says only that somebody holds it.  Whoever takes
//  it here says so, so that the others know it was this process.
//  netmux_take_driver() registers when nobody holds it and answers whether
//  this process is the driver now.
extern "C" bool netmux_take_driver();
extern "C" bool netmux_is_driver();

//  Hands c/libcr2 its frames through the above (net_set_frame_source).
void netmux_install_cr2();
