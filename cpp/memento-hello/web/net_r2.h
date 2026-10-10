#pragma once

#include "loader.h"

namespace web {

//
//  The r2 network stack, as the browser's NetIf.
//
//  There is one per process: the kernel queues frames for a process, not for
//  a window, so two stacks in one process would take each other's frames.
//  Memento's netmux routes frames between this stack (the Video window) and
//  the c/libcr2 stack (Chat/IRC). Hosted programs (r2web, jug, telegram)
//  have their own processes and port ranges.
//
NetIf &r2Net();
// Call before the first network operation; hosted processes have disjoint ports.
void r2NetSetPortBase(uint16_t port);

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
