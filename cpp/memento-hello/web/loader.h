#pragma once

#include "http.h"

struct web_tls;

namespace web {

//
//  What the loader needs from a network: name resolution and TCP streams,
//  none of it blocking.  Every call returns at once and the loader asks again
//  on its next step.  The r2 stack implements this (net_r2.cpp), and so does
//  the host test with ordinary sockets.
//
class NetIf
{
public:
    virtual ~NetIf() {}

    //  Moves the network along: takes in what has arrived, runs the timers.
    virtual void poll() = 0;

    //  1 with ip filled in, 0 while still waiting, -1 on failure.
    virtual int resolve(const char *host, uint8_t ip[4]) = 0;

    //  A connection handle, or -1.
    virtual int connect(const uint8_t ip[4], uint16_t port) = 0;

    enum
    {
        CONNECTING = 0,
        OPEN = 1,
        PEER_CLOSED = 2, // the peer is done sending; what arrived can still be read
        FAILED = -1,
    };
    virtual int status(int h) = 0;

    //  How many bytes were taken; 0 when there is no room just now.
    virtual size_t send(int h, const uint8_t *data, size_t n) = 0;
    virtual size_t recv(int h, uint8_t *data, size_t n) = 0;
    virtual void close(int h) = 0;

    //  Why a resolve or a connection failed.
    virtual const char *lastError() = 0;
};

//
//  One page load: resolve, connect, TLS if asked for, send the request, read
//  the response, and follow redirects.  step() does as much as it can without
//  waiting and returns; the window calls it from its idle loop.
//
class Loader
{
public:
    enum Phase
    {
        IDLE,
        RESOLVING,
        CONNECTING,
        HANDSHAKE,
        REQUEST,
        RESPONSE,
        DONE,
        FAILED,
    };

    //  The platform's entropy and clock, for TLS.
    typedef void (*SeedFn)(uint8_t *out, size_t n);
    typedef void (*TimeFn)(unsigned long *days, unsigned long *seconds);

    Loader(NetIf &net, SeedFn seed, TimeFn time);
    ~Loader();

    //  A GET, or with a body a POST of a urlencoded form.
    void start(const Url &u, bool insecure, const uint8_t *postBody = nullptr, size_t postLen = 0);
    void step();
    void cancel();

    Phase phase() const { return phase_; }
    bool busy() const { return phase_ != IDLE && phase_ != DONE && phase_ != FAILED; }

    //  One line for the status bar.
    const char *status() const { return status_; }

    //  Why it failed; and whether the failure was an unknown root, which the
    //  user may choose to get past.
    const char *error() const { return error_; }
    bool untrusted() const { return untrusted_; }

    //  The URL finally loaded, after redirects.
    const Url &url() const { return url_; }

    HttpResponse &response() { return resp_; }

    //  Bytes received so far on the current response, for the status bar.
    size_t received() const { return received_; }

    //  Where the time of the last load went, in milliseconds: "dns 40, connect
    //  30, tls 900, wait 200, body 1500".  Redirects add to each.
    void timing(char *out, size_t cap) const;

private:
    NetIf &net_;
    SeedFn seed_;
    TimeFn time_;

    Phase phase_ = IDLE;
    Url url_;
    bool insecure_ = false;
    int redirects_ = 0;

    uint8_t ip_[4] = {};
    int conn_ = -1;
    web_tls *tls_ = nullptr;

    Buf request_;
    size_t reqSent_ = 0;
    bool post_ = false;
    Buf postBody_;

    HttpResponse resp_;
    size_t received_ = 0;

    uint64_t phaseStart_ = 0;
    uint64_t spent_[FAILED + 1] = {};
    uint64_t responseAt_ = 0, firstByteAt_ = 0; // of the last response
    uint64_t lastActivity_ = 0;

    char status_[96] = {};
    char error_[160] = {};
    bool untrusted_ = false;

    void begin();
    void setPhase(Phase p, const char *what);
    void fail(const char *what, const char *detail = nullptr);
    void teardown();
    void finishResponse();
    void pumpPlain();
    void pumpTls();
};

} // namespace web
