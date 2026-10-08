//
//  fetch.h --- one file over HTTP(S), with Memento's web engine underneath:
//  its TCP/IP stack (net_r2.cpp), DNS, TLS through BearSSL, redirects.
//
//  Nothing blocks.  step() moves the transfer along and returns; the command
//  line calls it in a loop, the window from its idle loop.
//
#pragma once

#include "loader.h"

namespace jug {

class Fetch
{
public:
    Fetch();

    //  False when `url` is no URL (error() says so).
    bool start(const char *url, bool insecure);
    //  True while the transfer is still under way.
    bool step();
    void cancel();

    bool busy() const { return loader_.busy(); }
    //  Finished, with a 200 and the whole of the body.
    bool ok() const;
    const char *error() const;
    //  "Connecting to cdn.vxn.dev...", "Receiving... 40 KiB"
    const char *status() const { return loader_.status(); }
    size_t received() const { return loader_.received(); }
    web::HttpResponse &response() { return loader_.response(); }

private:
    web::Loader loader_;
    mutable char error_[192] = {};
};

//  The local ports the network stack takes its connections from.  The
//  command line and the window use different ones, so both can run at once.
void use_ports(uint16_t base);

//  Keeps the network turning for `ms`, so the last connection's close gets
//  out before the process ends.
void linger(uint32_t ms);

} // namespace jug
