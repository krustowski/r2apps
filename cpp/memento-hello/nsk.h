//
// nsk — host discovery for the Network window, after c/nsk.
//
// c/nsk sweeps a subnet from the shell: an ARP who-has for every address in
// it, then an ICMP echo to every address that stayed silent, and a listing of
// what answered.  It owns the process, so it can wait for replies.  This is
// the same sweep turned inside out for a window: start() sets it going,
// step() --- from the window's idle loop --- sends the next few probes and
// takes in the replies, and never waits.  Frames come through netmux (as
// NETMUX_NSK), so a scan does not take the browser's or the chat's.
//
// What c/nsk's README says about the kernel holds here too: ARP and echo
// replies go only to the process registered as the global Ethernet driver.
// When this process can take that registration (nobody holds it, or the
// browser took it) the scan is c/nsk's.  When another process holds it ---
// eth.elf --- echo replies go there, but the kernel copies every ARP reply to
// each process with a TCP port bound: so the scan binds one (NSK_PORT) and
// sweeps with ARP alone, which reaches the local link only.
//
#pragma once

#include <r2/types.hpp>

class HostScan
{
public:
    //  c/nsk's limits: /22 at the widest, a second of listening per sweep.
    static const int MAX_TARGETS = 1024;
    static const uint32_t TIMEOUT_MS = 1000;
    //  Bound, when another process is the driver, for the ARP replies the
    //  kernel copies to port owners.  Nothing is ever sent to it.
    static const uint16_t NSK_PORT = 0x4e53;

    enum State
    {
        IDLE,
        ARP_SWEEP,
        ARP_WAIT,
        ICMP_SWEEP,
        ICMP_WAIT,
        DONE,
        FAILED,
    };

    struct Host
    {
        uint8_t mac[6];
        uint8_t flags;   // F_* below
        uint8_t pad;
        uint32_t sentMs; // when the last probe went, for the round trip
        uint32_t rttMs;
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

    //  The machine's own address, MAC, and the subnet to offer for a scan
    //  ("10.3.4.0/24"): from the driver's configuration, the netmask narrowed
    //  to /22 when it is wider.  `cidr` is at least 20 bytes.
    static void suggest(char *cidr);

    //  Parses `cidr` and starts sweeping it.  False, with error() saying why,
    //  when the range does not parse, is too wide, or the NIC is not ours.
    bool start(const char *cidr);
    void stop();
    //  Sends and receives what is due now; true when something a window
    //  shows has changed.
    bool step();

    State state() const { return state_; }
    bool busy() const { return state_ != IDLE && state_ != DONE && state_ != FAILED; }
    //  Another process drives the NIC: ARP only, no ping.
    bool arpOnly() const { return arpOnly_; }
    const char *error() const { return error_; }
    //  One line of progress: "arp sweep 40/254", "3 up / 254 in 2010 ms".
    void describe(char *out, size_t cap) const;

    int count() const { return n_; }
    int up() const { return up_; }
    //  The i-th address of the range, and what the scan found there.
    void addressOf(int i, uint8_t ip[4]) const;
    const Host &host(int i) const { return hosts_[i]; }
    bool isUp(int i) const { return hosts_[i].flags & (F_ARP | F_ICMP | F_SELF); }

private:
    State state_ = IDLE;
    char error_[64] = {};
    char cidr_[20] = {};

    Host hosts_[MAX_TARGETS];
    int n_ = 0, up_ = 0;
    uint32_t base_ = 0;  // the address of hosts_[0]
    int next_ = 0;       // the next to probe in the sweep under way
    bool doArp_ = false, doIcmp_ = false;
    uint64_t t0_ = 0, waitUntil_ = 0, doneMs_ = 0;

    uint8_t ip_[4] = {};
    uint8_t mac_[6] = {};
    uint8_t gw_[4] = {};
    uint8_t gwMac_[6] = {};
    bool gwMacKnown_ = false;
    bool onLink_ = true;
    bool arpOnly_ = false;

    uint8_t rx_[2048];
    uint8_t tx_[128];

    int indexOf(const uint8_t ip[4]) const;
    void markUp(int idx, bool viaArp, const uint8_t *mac, bool solicited);
    bool onFrame(const uint8_t *f, size_t n);
    void sendArp(uint16_t op, const uint8_t tip[4], const uint8_t tmac[6]);
    void sendEcho(int idx);
    void sendFrame(size_t len);
    void fail(const char *why);
};
