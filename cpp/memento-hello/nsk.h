//
// nsk — the Network window's tools: host discovery after c/nsk, ping and
// traceroute.
//
// c/nsk sweeps a subnet from the shell: an ARP who-has for every address in
// it, then an ICMP echo to every address that stayed silent, and a listing of
// what answered.  It owns the process, so it can wait for replies.  Here the
// same work is turned inside out for a window: start() sets a tool going,
// step() --- from the window's idle loop --- sends what is due, onFrame()
// takes in the replies, and nothing ever waits.  Frames come through netmux
// (as NETMUX_NSK), so the tools do not take the browser's or the chat's.
//
// What c/nsk's README says about the kernel holds here too: ICMP replies go
// only to the process registered as the global Ethernet driver.  When this
// process can take that registration (nobody holds it, or the browser took
// it) every tool works as it does in c/nsk.  When another process holds it ---
// eth.elf --- echo replies and the routers' errors go there, but the kernel
// copies every ARP reply to each process with a TCP port bound, and TCP to a
// bound port comes to its owner: so the tools bind one (NetLink::PORT) while
// they work, and give it back when they stop.  The scan then sweeps with ARP
// alone, which reaches the local link only; ping uses ARP on the link and a
// TCP SYN beyond it; traceroute, which lives on the routers' ICMP, cannot run.
//
#pragma once

#include <r2/types.hpp>

//
//  The tools' way onto the wire: the machine's address, MAC, netmask and
//  gateway as the driver published them, the port or the driver registration
//  that brings replies here, the MACs heard so far, and frames out.
//
class NetLink
{
public:
    //  Bound while a tool works and another process drives the NIC.
    static const uint16_t PORT = 0x4e53;

    //  Ready to send and to hear: this process the driver, when nobody was,
    //  or PORT bound.  False, with why, when there is no card or no port.
    ~NetLink() { close(); }
    bool open(char *why, size_t cap);
    //  Gives PORT back.  A driver registration stays: there is no syscall to
    //  drop one, and the browser may be using it.
    void close();
    bool isOpen() const { return open_; }
    //  ICMP replies come here: ping and traceroute speak ICMP.
    bool driver() const { return driver_; }
    //  Reads the driver's configuration again.
    void refresh();

    const uint8_t *ip() const { return ip_; }
    const uint8_t *mac() const { return mac_; }
    //  On the local link: inside the published netmask's subnet, or the
    //  local /24 when none is published.
    bool onLink(const uint8_t ip[4]) const;
    //  Where a frame for `dst` goes first: itself on the link, else the
    //  gateway.
    void nextHop(const uint8_t dst[4], uint8_t hop[4]) const;
    //  A MAC known without asking: heard in an ARP reply, or the gateway's
    //  from the driver's configuration.
    bool knownMac(const uint8_t ip[4], uint8_t mac[6]) const;

    //  An ARP frame for the cache, and when this process is the driver an
    //  answer to a request for our address (the browser answers them while it
    //  runs, but it may not be running).
    void onFrame(const uint8_t *f, size_t n);

    void sendArp(uint16_t op, const uint8_t tip[4], const uint8_t tmac[6]);
    void sendEcho(const uint8_t dmac[6], const uint8_t dip[4], uint16_t id, uint16_t seq, uint8_t ttl);
    void sendTcp(const uint8_t dmac[6], const uint8_t dip[4], uint16_t sport, uint16_t dport, uint32_t seq,
                 uint32_t ack, uint8_t flags);

    //  "10.3.4.2" into ip; false when it is not one.
    static bool parseAddress(const char *s, uint8_t ip[4]);
    //  "10.3.4.2" from ip, NUL-terminated (16 bytes).
    static void formatAddress(const uint8_t ip[4], char *out);

private:
    bool open_ = false, driver_ = false, bound_ = false;
    uint8_t ip_[4] = {}, mask_[4] = {}, mac_[6] = {}, gw_[4] = {}, gwMac_[6] = {};
    bool gwMacKnown_ = false;
    uint16_t ipId_ = 0;

    struct Seen
    {
        uint8_t ip[4];
        uint8_t mac[6];
        bool used;
    };
    Seen seen_[16] = {};
    int seenNext_ = 0;
    void learn(const uint8_t ip[4], const uint8_t mac[6]);

    uint8_t tx_[128];
    void ipHeader(uint8_t *ip, const uint8_t dip[4], uint8_t proto, uint16_t len, uint8_t ttl);
};

//
//  Host discovery.  Each address is asked up to ARP_TRIES times by ARP and,
//  when this process gets ICMP, by echo after that --- a phone asleep on
//  Wi-Fi misses a single who-has as often as not, and one probe each is
//  what made such hosts come and go between scans.  A round asks only the
//  addresses not heard yet, paced at PROBES_PER_MS, with GAP_MS between
//  rounds for late answers, and the scan listens LISTEN_MS after the last.
//
class HostScan
{
public:
    //  c/nsk's limit: /22 at the widest.
    static const int MAX_TARGETS = 1024;
    static const int ARP_TRIES = 3;
    static const int PING_TRIES_ON_LINK = 1; // ARP is the real test there
    static const int PING_TRIES_OFF_LINK = 2;
    static const int PROBES_PER_MS = 1, PACE = 16;
    static const uint32_t GAP_MS = 600;
    static const uint32_t LISTEN_MS = 1500;

    enum State
    {
        IDLE,
        SWEEP,
        WAIT,
        DONE,
        FAILED,
    };

    struct Host
    {
        uint8_t mac[6];
        uint8_t flags;      // F_* below
        uint8_t tries;      // probes sent to it
        uint32_t sentMs;    // when the last went, for the round trip
        uint32_t rttMs;
        uint8_t answeredOn; // which probe the first answer came to
        uint8_t pad[3];
    };
    enum
    {
        F_MAC = 1,
        F_ARP = 2,
        F_ICMP = 4,
        F_SELF = 8,
        F_SENT = 16,
        F_RTT = 32,
    };

    //  The subnet to offer for a scan ("10.3.4.0/24"): from the driver's
    //  configuration, the netmask narrowed to /22 when it is wider.  `cidr`
    //  is at least 20 bytes.
    static void suggest(char *cidr);

    //  Parses `cidr` and starts sweeping it.  False, with error() saying why,
    //  when the range does not parse, is too wide, or nothing can be heard.
    bool start(NetLink &link, const char *cidr);
    void stop();
    //  Sends what is due at `now`; true when something a window shows has
    //  changed.
    bool step(uint64_t now);
    bool onFrame(const uint8_t *f, size_t n);

    State state() const { return state_; }
    bool busy() const { return state_ == SWEEP || state_ == WAIT; }
    //  Another process drives the NIC: ARP only, no ping.
    bool arpOnly() const { return arpOnly_; }
    const char *error() const { return error_; }
    //  One line of progress: "arp 2/3: 40/254, 3 up", "3 up / 254 in 3410 ms".
    void describe(char *out, size_t cap) const;

    int count() const { return n_; }
    int up() const { return up_; }
    //  The i-th address of the range, and what the scan found there.
    void addressOf(int i, uint8_t ip[4]) const;
    const Host &host(int i) const { return hosts_[i]; }
    bool isUp(int i) const { return hosts_[i].flags & (F_ARP | F_ICMP | F_SELF); }

private:
    NetLink *link_ = nullptr;
    State state_ = IDLE;
    char error_[64] = {};

    Host hosts_[MAX_TARGETS];
    int n_ = 0, up_ = 0;
    uint16_t seqBase_ = 0;
    uint32_t base_ = 0;  // the address of hosts_[0]
    bool arpPhase_ = true;
    int round_ = 0;      // within the phase, from 1
    int next_ = 0;       // the next to probe in the round under way
    int sent_ = 0;       // probes in the round so far
    bool doArp_ = false, doIcmp_ = false;
    bool onLink_ = true, arpOnly_ = false;
    uint64_t t0_ = 0, roundStart_ = 0, waitUntil_ = 0, doneMs_ = 0;

    int indexOf(const uint8_t ip[4]) const;
    int tries() const;
    void markUp(int idx, bool viaArp, const uint8_t *mac, bool current);
    void sendEcho(int idx);
    void fail(const char *why);
};

//
//  Ping: a probe a second until stopped, each answered or timed out within
//  TIMEOUT_MS.  ICMP echo when this process gets ICMP; otherwise an ARP
//  who-has to a host on the link, or a TCP SYN to port 80 beyond it, whose
//  SYN/ACK or RST says the host is there just as well.
//
class Pinger
{
public:
    enum Mode
    {
        NONE,
        ICMP,
        ARP,
        TCP,
    };
    enum Kind
    {
        L_REPLY,   // an echo or ARP reply
        L_OPEN,    // TCP: SYN/ACK, port 80 open
        L_CLOSED,  // TCP: RST, nothing on port 80 but the host is there
        L_TIMEOUT, // no answer
        L_UNREACH, // a router said it cannot get there
        L_NOARP,   // the next hop never answered ARP: nothing was sent
    };
    struct Line
    {
        uint16_t seq;
        uint8_t kind;
        uint8_t ttl; // of the reply's IP header, 0 for ARP
        uint32_t ms;
        uint8_t from[4];
    };
    static const int LOG = 64; // lines kept
    static const uint32_t INTERVAL_MS = 1000, TIMEOUT_MS = 2000;
    static const uint16_t TCP_PORT = 80;

    bool start(NetLink &link, const uint8_t dst[4]);
    void stop();
    bool busy() const { return busy_; }
    bool step(uint64_t now);
    bool onFrame(const uint8_t *f, size_t n);

    Mode mode() const { return mode_; }
    const char *error() const { return error_; }
    const uint8_t *target() const { return dst_; }
    //  How it pings and why: "ICMP echo", "ARP: ..., on the link".
    void describe(char *out, size_t cap) const;
    //  "4 sent, 4 back, 0% lost, 1/2/4 ms".
    void summary(char *out, size_t cap) const;

    int lines() const { return nLines_ < LOG ? nLines_ : LOG; }
    //  The i-th line kept, oldest first.
    const Line &line(int i) const;

private:
    struct Probe
    {
        uint16_t seq;
        bool live;
        uint32_t sentMs;
        uint32_t tcpSeq;
    };
    NetLink *link_ = nullptr;
    bool busy_ = false;
    Mode mode_ = NONE;
    char error_[64] = {};
    uint8_t dst_[4] = {}, hop_[4] = {}, dmac_[6] = {};
    bool haveMac_ = false;
    uint16_t seq_ = 0, seqBase_ = 0;
    uint64_t resolveAt_ = 0, resolveUntil_ = 0;
    uint64_t nextSend_ = 0;
    Probe pend_[4] = {};
    Line log_[LOG];
    int nLines_ = 0;
    int sent_ = 0, back_ = 0;
    uint32_t minMs_ = 0, maxMs_ = 0;
    uint64_t sumMs_ = 0;

    void add(uint16_t seq, Kind kind, uint8_t ttl, uint32_t ms, const uint8_t from[4]);
    Probe *pending(uint16_t seq);
    void answer(Probe &p, Kind kind, uint8_t ttl, const uint8_t from[4]);
};

//
//  Traceroute: ICMP echoes with a TTL of 1, 2, 3 ... and the routers' time
//  exceeded errors say who is at each hop, the destination's echo reply says
//  where it ends.  ROUNDS rounds of one probe per hop, sent SPACING_MS apart
//  rather than a hop at a time, so the whole path takes a few seconds.  Only
//  when this process gets ICMP.
//
class Tracer
{
public:
    static const int MAX_HOPS = 30, ROUNDS = 3;
    static const uint32_t SPACING_MS = 20, ROUND_WAIT_MS = 2000;

    enum
    {
        H_ADDR = 1,    // someone answered for this hop
        H_DEST = 2,    // the destination itself
        H_UNREACH = 4, // an unreachable error: the path ends here
    };
    struct Hop
    {
        uint8_t ip[4];
        uint8_t flags;
        uint8_t code;     // of the unreachable error
        uint8_t answered; // a bit per round
        uint8_t pad;
        uint32_t sentMs[ROUNDS];
        uint32_t ms[ROUNDS];
    };

    bool start(NetLink &link, const uint8_t dst[4]);
    void stop();
    bool busy() const { return state_ == RESOLVE || state_ == SEND || state_ == WAIT; }
    bool step(uint64_t now);
    bool onFrame(const uint8_t *f, size_t n);

    const char *error() const { return error_; }
    const uint8_t *target() const { return dst_; }
    //  "round 2/3, hop 5", "1.1.1.1 in 7 hops".
    void describe(char *out, size_t cap) const;
    //  Rows to show: up to the destination when it answered, else as far as
    //  probes went.
    int hops() const;
    const Hop &hop(int i) const { return hops_[i]; } // i = TTL - 1

private:
    enum State
    {
        IDLE,
        RESOLVE,
        SEND,
        WAIT,
        DONE,
        FAILED,
    };
    NetLink *link_ = nullptr;
    State state_ = IDLE;
    char error_[64] = {};
    uint8_t dst_[4] = {}, hop_[4] = {}, dmac_[6] = {};
    Hop hops_[MAX_HOPS] = {};
    uint16_t seqBase_ = 0;
    int round_ = 0, ttl_ = 0, sentTo_ = 0;
    int end_ = MAX_HOPS; // the hop where the path ends, once known
    int arpTries_ = 0;
    uint64_t nextAt_ = 0;

    void fail(const char *why);
    bool roundAnswered() const;
};

//
//  The four together, and the frames they share: one pump from netmux for
//  all of them, and the port given back as soon as none is at work.
//
class NetTools
{
public:
    ~NetTools() { stopAll(); }
    NetLink link;
    HostScan scan;
    Pinger ping;
    Tracer trace;

    bool busy() const { return scan.busy() || ping.busy() || trace.busy(); }
    //  Takes in what has come, sends what is due; true when anything a
    //  window shows has changed.
    bool step();
    void stopAll();

private:
    uint8_t rx_[2048];
};
