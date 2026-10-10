//
//  host.h --- how Memento's Jug window and jug.elf --host meet.
//
//  The same shared block, frames and input queue as r2web's (../r2web/host.h):
//  Memento allocates it on the kernel's user heap, starts `jug.elf --host
//  0x<address>`, passes the window's keys and clicks in and shows the frames
//  jug draws.  Its own magic, so that neither program takes the other's block.
//
#pragma once

#include "../r2web/host.h"

namespace jughost {
constexpr uint32_t Magic = 0x3147554a; // "JUG1"
//  The local ports of the window's connections; the command line has 46000.
constexpr uint32_t PortBase = 46100;
//  attentionPending: this flag plus the number of fresh builds asks Memento
//  for a clock notification as well as attention. Legacy value 1 asks only
//  for attention; older hosts treat either value as an attention request.
constexpr uint32_t UpdateNotification = 1u << 31;
} // namespace jughost
