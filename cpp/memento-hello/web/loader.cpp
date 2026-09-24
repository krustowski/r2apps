#include "loader.h"
#include "tls.h"

namespace web {

//  How long each phase may take before the load is given up.  Generous: the
//  kernel delivers one frame per tick, and the handshake's public-key maths is
//  slow on an emulated CPU.
static const uint64_t RESOLVE_MS = 15000;
static const uint64_t CONNECT_MS = 15000;
static const uint64_t HANDSHAKE_MS = 30000;
static const uint64_t IDLE_MS = 30000;

static const int MAX_REDIRECTS = 8;

Loader::Loader(NetIf &net, SeedFn seed, TimeFn time) : net_(net), seed_(seed), time_(time) {}

Loader::~Loader() { teardown(); }

void Loader::setPhase(Phase p, const char *what)
{
    uint64_t now = now_ms();
    if (phase_ != IDLE && phaseStart_)
        spent_[phase_] += now - phaseStart_;
    phase_ = p;
    phaseStart_ = lastActivity_ = now;
    if (p == RESPONSE)
    {
        responseAt_ = now;
        firstByteAt_ = 0;
    }
    scopy(status_, what, sizeof(status_));
}

void Loader::fail(const char *what, const char *detail)
{
    teardown();
    scopy(error_, what, sizeof(error_));
    if (detail && detail[0])
    {
        scat(error_, ": ", sizeof(error_));
        scat(error_, detail, sizeof(error_));
    }
    phase_ = FAILED;
    scopy(status_, "Failed", sizeof(status_));
}

void Loader::teardown()
{
    if (tls_)
    {
        web_tls_free(tls_);
        tls_ = nullptr;
    }
    if (conn_ >= 0)
    {
        net_.close(conn_);
        conn_ = -1;
    }
}

void Loader::cancel()
{
    if (!busy())
        return;
    fail("Stopped");
}

void Loader::start(const Url &u, bool insecure, const uint8_t *postBody, size_t postLen)
{
    post_ = postBody != nullptr;
    postBody_.clear();
    if (post_ && postLen)
        postBody_.append(postBody, postLen);
    teardown();
    url_ = u;
    insecure_ = insecure;
    redirects_ = 0;
    untrusted_ = false;
    phase_ = IDLE;
    phaseStart_ = 0;
    memset(spent_, 0, sizeof(spent_));
    error_[0] = 0;
    begin();
}

void Loader::begin()
{
    resp_.reset();
    received_ = 0;
    reqSent_ = 0;
    if (!httpBuildRequest(url_, postBody_.data, postBody_.len, post_, request_))
    {
        fail("The address is too long");
        return;
    }

    if (parseIPv4(url_.host, ip_))
    {
        conn_ = net_.connect(ip_, url_.port);
        if (conn_ < 0)
        {
            fail("Could not open a connection", net_.lastError());
            return;
        }
        setPhase(CONNECTING, "Connecting...");
        return;
    }
    char s[96] = "Looking up ";
    scat(s, url_.host, sizeof(s));
    scat(s, "...", sizeof(s));
    setPhase(RESOLVING, s);
}

void Loader::timing(char *out, size_t cap) const
{
    static const char *const names[] = {"", "dns ", "connect ", "tls ", "send ", "response ", "", ""};
    out[0] = 0;
    for (int p = RESOLVING; p <= RESPONSE; p++)
    {
        if (!spent_[p])
            continue;
        if (out[0])
            scat(out, ", ", cap);
        scat(out, names[p], cap);
        scatInt(out, (long)spent_[p], cap);
    }
    scat(out, " ms", cap);
    if (firstByteAt_ && responseAt_ && firstByteAt_ >= responseAt_)
    {
        scat(out, " (first byte after ", cap);
        scatInt(out, (long)(firstByteAt_ - responseAt_), cap);
        scat(out, ")", cap);
    }
}

void Loader::finishResponse()
{
    if (resp_.failed)
    {
        fail("Bad response from the server", resp_.error);
        return;
    }

    if (resp_.isRedirect())
    {
        if (++redirects_ > MAX_REDIRECTS)
        {
            fail("Too many redirects");
            return;
        }
        Url next;
        if (!urlResolve(url_, resp_.location, next))
        {
            fail("Redirected to an address this browser cannot open", resp_.location);
            return;
        }
        teardown();
        url_ = next;
        //  After a POST: 307 and 308 repeat it, the others fetch with GET.
        if (resp_.status != 307 && resp_.status != 308)
        {
            post_ = false;
            postBody_.clear();
        }
        begin();
        return;
    }

    teardown();
    spent_[phase_] += now_ms() - phaseStart_;
    phase_ = DONE;
    scopy(status_, "Done", sizeof(status_));
}

void Loader::pumpPlain()
{
    uint64_t now = now_ms();

    if (phase_ == REQUEST)
    {
        size_t n = net_.send(conn_, request_.data + reqSent_, request_.len - reqSent_);
        reqSent_ += n;
        if (reqSent_ >= request_.len)
            setPhase(RESPONSE, "Waiting for the server...");
        return;
    }

    //  RESPONSE
    uint8_t buf[2048];
    for (int k = 0; k < 16; k++)
    {
        size_t n = net_.recv(conn_, buf, sizeof(buf));
        if (!n)
            break;
        lastActivity_ = now;
        if (!received_)
            firstByteAt_ = now_ms();
        received_ += n;
        resp_.feed(buf, n);
        if (resp_.done || resp_.failed)
        {
            finishResponse();
            return;
        }
    }

    int st = net_.status(conn_);
    if (st == NetIf::PEER_CLOSED || st == NetIf::FAILED)
    {
        //  Everything readable has been read above; this is the end.
        if (st == NetIf::FAILED && !resp_.headersDone)
        {
            fail("The connection failed", net_.lastError());
            return;
        }
        resp_.onEof();
        finishResponse();
    }
}

void Loader::pumpTls()
{
    uint64_t now = now_ms();

    for (int iter = 0; iter < 64; iter++)
    {
        unsigned st = web_tls_state(tls_);

        if (st & WEB_TLS_CLOSED)
        {
            int err = web_tls_error(tls_);
            if (err == 0 || (phase_ == RESPONSE && resp_.headersDone))
            {
                resp_.onEof();
                finishResponse();
                return;
            }
            untrusted_ = web_tls_error_is_untrusted(err) != 0;
            char detail[128];
            scopy(detail, web_tls_error_text(err), sizeof(detail));
            if (untrusted_ && web_tls_anchor_count() <= 0)
            {
                //  Not this server's fault: there was nothing to check it against.
                scopy(detail, "no trusted roots could be read from ", sizeof(detail));
                scat(detail, web_tls_platform_anchor_path(), sizeof(detail));
            }
            scat(detail, " (", sizeof(detail));
            scatInt(detail, err, sizeof(detail));
            scat(detail, ")", sizeof(detail));
            fail("Secure connection failed", detail);
            return;
        }

        bool progress = false;

        if (st & WEB_TLS_SENDREC)
        {
            unsigned long len = 0;
            unsigned char *buf = web_tls_sendrec_buf(tls_, &len);
            size_t n = net_.send(conn_, buf, len);
            if (n)
            {
                web_tls_sendrec_ack(tls_, n);
                progress = true;
            }
        }

        if (st & WEB_TLS_RECVAPP)
        {
            unsigned long len = 0;
            unsigned char *buf = web_tls_recvapp_buf(tls_, &len);
            if (!received_)
                firstByteAt_ = now_ms();
            resp_.feed(buf, len);
            received_ += len;
            web_tls_recvapp_ack(tls_, len);
            lastActivity_ = now;
            progress = true;
            if (resp_.done || resp_.failed)
            {
                finishResponse();
                return;
            }
        }

        if ((st & WEB_TLS_SENDAPP) && reqSent_ < request_.len)
        {
            if (phase_ == HANDSHAKE)
                setPhase(REQUEST, "Sending the request...");
            unsigned long len = 0;
            unsigned char *buf = web_tls_sendapp_buf(tls_, &len);
            size_t n = request_.len - reqSent_;
            if (n > len)
                n = len;
            memcpy(buf, request_.data + reqSent_, n);
            web_tls_sendapp_ack(tls_, n);
            reqSent_ += n;
            if (reqSent_ >= request_.len)
            {
                web_tls_flush(tls_);
                setPhase(RESPONSE, "Waiting for the server...");
            }
            progress = true;
        }

        if (st & WEB_TLS_RECVREC)
        {
            unsigned long len = 0;
            unsigned char *buf = web_tls_recvrec_buf(tls_, &len);
            size_t n = net_.recv(conn_, buf, len);
            if (n)
            {
                web_tls_recvrec_ack(tls_, n);
                lastActivity_ = now;
                progress = true;
            }
            else
            {
                int ns = net_.status(conn_);
                if (ns == NetIf::PEER_CLOSED || ns == NetIf::FAILED)
                {
                    //  Plenty of servers close without a close_notify once the
                    //  body is sent.  The HTTP framing says whether anything
                    //  is missing, so that is what decides.
                    if (phase_ == RESPONSE && resp_.headersDone)
                    {
                        resp_.onEof();
                        finishResponse();
                    }
                    else
                        fail("The server closed the connection during the handshake",
                             ns == NetIf::FAILED ? net_.lastError() : nullptr);
                    return;
                }
            }
        }

        if (!progress)
            break;
    }
}

void Loader::step()
{
    if (!busy())
        return;

    net_.poll();
    uint64_t now = now_ms();

    switch (phase_)
    {
    case RESOLVING:
    {
        int r = net_.resolve(url_.host, ip_);
        if (r < 0)
        {
            fail("Could not find the server", net_.lastError());
            return;
        }
        if (r == 0)
        {
            if (now - phaseStart_ > RESOLVE_MS)
                fail("Could not find the server", "the name server did not answer");
            return;
        }
        conn_ = net_.connect(ip_, url_.port);
        if (conn_ < 0)
        {
            fail("Could not open a connection", net_.lastError());
            return;
        }
        char s[96] = "Connecting to ";
        scat(s, url_.host, sizeof(s));
        scat(s, "...", sizeof(s));
        setPhase(CONNECTING, s);
        return;
    }

    case CONNECTING:
    {
        int st = net_.status(conn_);
        if (st == NetIf::FAILED || st == NetIf::PEER_CLOSED)
        {
            fail("Could not connect", net_.lastError());
            return;
        }
        if (st == NetIf::CONNECTING)
        {
            if (now - phaseStart_ > CONNECT_MS)
                fail("Could not connect", "no answer from the server");
            return;
        }
        if (url_.https)
        {
            //  Raw material, not finished randomness: the engine condenses it
            //  with HMAC-DRBG, so more is better when each byte is weak.
            uint8_t seed[512];
            seed_(seed, sizeof(seed));
            unsigned long days = 0, secs = 0;
            time_(&days, &secs);
            tls_ = web_tls_new(url_.host, insecure_ ? 1 : 0, seed, sizeof(seed), days, secs);
            memset(seed, 0, sizeof(seed));
            if (!tls_)
            {
                fail("Out of memory for the secure connection");
                return;
            }
            setPhase(HANDSHAKE, "Securing the connection...");
            pumpTls();
        }
        else
        {
            setPhase(REQUEST, "Sending the request...");
            pumpPlain();
        }
        return;
    }

    case HANDSHAKE:
    case REQUEST:
    case RESPONSE:
        if (tls_)
            pumpTls();
        else
            pumpPlain();
        if (!busy())
            return;
        //  Decrypting a record can take longer than a tick; whatever arrived
        //  meanwhile is taken in now, before the window's repaint and the
        //  loop's sleep give the kernel time to overwrite it.
        net_.poll();
        //  The time after the pump, not before it: the handshake's arithmetic
        //  takes long enough on an emulated CPU that a phase can start in the
        //  middle of this step, and "now" from before it would be earlier
        //  than the phase's own start --- which, unsigned, is a very long wait.
        now = now_ms();
        if (phase_ == HANDSHAKE && now - phaseStart_ > HANDSHAKE_MS)
            fail("Secure connection failed", "the handshake timed out");
        else if (now - lastActivity_ > IDLE_MS)
            fail("The server stopped answering");
        else if (phase_ == RESPONSE && received_)
        {
            char s[96] = "Receiving... ";
            scatInt(s, (long)(received_ / 1024), sizeof(s));
            scat(s, " KiB", sizeof(s));
            scopy(status_, s, sizeof(status_));
        }
        return;

    default:
        return;
    }
}

} // namespace web
