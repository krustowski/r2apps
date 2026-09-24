#pragma once

#include "loader.h"

namespace web {

//
//  The r2 network stack, as the browser's NetIf.
//
//  There is one per process: the kernel queues frames for a process, not for
//  a window, so two stacks in one process would take each other's frames.
//  The same goes for c/libcr2's stack, which the chat and IRC windows use ---
//  with one of those connected in the same Memento session, whichever polls
//  first gets the frame.  Use one or the other at a time.
//
NetIf &r2Net();

//  "10.3.4.2 via 10.3.4.1, DNS 1.1.1.1, driver" --- for the about page.
void r2NetDescribe(char *out, size_t cap);

//  Settings from the address bar (":dns 9.9.9.9", ":gw 10.3.4.1").  Return
//  false when the argument is not an address.
bool r2NetSetDns(const char *ip);
bool r2NetSetGateway(const char *ip);

//  For the loader: raw entropy for the TLS engine, and the date (web_r2.cpp).
void gatherEntropy(uint8_t *out, size_t n);
void currentTime(unsigned long *days, unsigned long *seconds);

} // namespace web
