//
//  host.h --- how Memento's Telegram window and telegram.elf --host meet.
//
//  The same shared block, frames and input queue as r2web's and jug's
//  (cpp/r2web/host.h): Memento allocates it on the kernel's user heap, starts
//  `telegram.elf --host 0x<address>`, passes the window's keys and clicks in
//  and shows the frames telegram.elf draws.  Its own magic, so that no other
//  program takes its block.  The Go side is package hosted
//  (go/libgor2/memento/hosted); the window asks for pictures on a Ctrl+V
//  (r2web::PasteImage), for the screenshots it sends.
//
#pragma once

#include "../../cpp/r2web/host.h"

namespace tghost {
constexpr uint32_t Magic = 0x31474554; // "TEG1"
} // namespace tghost
