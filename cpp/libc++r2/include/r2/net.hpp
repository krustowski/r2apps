#ifndef _R2CXX_NET_HPP_
#define _R2CXX_NET_HPP_

/*
 *  net.hpp — the networking syscalls.
 *
 *  This is the ABI layer, not a protocol stack: addresses, byte order, the
 *  frame in and out calls, and the registration a process needs before the
 *  kernel will deliver anything to it.  c/libcr2's net.c already implements
 *  ARP, ICMP, DHCP and TCP on top of exactly these calls, and a C++ program
 *  that wants TCP should link it rather than have this library grow a second
 *  copy --- see README.md, "Using libcr2 from C++".
 *
 *  Registration comes in two kinds.  register_driver() claims every frame the
 *  kernel cannot route elsewhere, which is what the ETH driver does; a normal
 *  program calls bind_port() instead and receives only TCP frames for that
 *  port, with ETH already running to answer ARP and ICMP.
 */

#include "optional.hpp"
#include "span.hpp"
#include "string.hpp"
#include "syscall.hpp"
#include "vector.hpp"

namespace r2::net {

/*  Packet kinds the kernel crafts (syscall 0x33) or delivers (0x35).  */
enum class PacketKind : uint8_t {
    Ipv4 = 0x01,
    Icmp = 0x02,
    Tcp = 0x03,
    EthernetFrame = 0x04,
};

inline constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
inline constexpr uint16_t ETHERTYPE_ARP = 0x0806;

/* ------------------------------------------------------------------------ *
 *  Byte order
 * ------------------------------------------------------------------------ */

inline constexpr uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
inline constexpr uint16_t ntohs(uint16_t v) { return htons(v); }
inline constexpr uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
inline constexpr uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

/*  The one's-complement checksum used by IPv4, ICMP, TCP and UDP.  */
uint16_t checksum(const_byte_span data);

/* ------------------------------------------------------------------------ *
 *  Addresses
 * ------------------------------------------------------------------------ */

struct MacAddress {
    uint8_t octets[6];

    string to_string() const; /*  "52:54:00:12:34:56"  */
    bool operator==(const MacAddress &other) const;
};

struct Ipv4 {
    uint8_t octets[4];

    constexpr Ipv4() : octets{0, 0, 0, 0} {}
    constexpr Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : octets{a, b, c, d} {}

    /*  Host byte order.  */
    constexpr uint32_t value() const {
        return ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
               ((uint32_t)octets[2] << 8) | octets[3];
    }

    static constexpr Ipv4 from_value(uint32_t v) {
        return Ipv4((uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
    }

    /*  Parses dotted-quad notation; nullopt if it is not one.  */
    static optional<Ipv4> parse(string_view text);

    string to_string() const;

    constexpr bool is_unspecified() const { return value() == 0; }
    bool operator==(const Ipv4 &other) const { return value() == other.value(); }
};

/* ------------------------------------------------------------------------ *
 *  Status and registration
 * ------------------------------------------------------------------------ */

struct Status {
    MacAddress mac;
    Ipv4 ip;
    bool driver_active;
    vector<uint16_t> bound_ports;
};

/*  Network status (syscall 0x38).  */
optional<Status> status();

/*  Waits until the ETH driver reports an address, or the timeout expires.  */
optional<Ipv4> wait_for_address(uint64_t timeout_ms = 15000);

/*  Claims every unrouted Ethernet frame and brings up the RTL8139 (0x37).  */
bool register_driver();

/*  Receives only TCP frames for this port (0x37 with the port as arg1).  */
bool bind_port(uint16_t port);

/* ------------------------------------------------------------------------ *
 *  Frames
 * ------------------------------------------------------------------------ */

/*  Blocks until a frame of this kind arrives; returns its length (0x35).  */
int64_t receive(PacketKind kind, byte_span buffer);

/*  Returns 0 immediately when nothing is queued.  */
int64_t receive_nonblocking(byte_span buffer);

/*  Sends a complete Ethernet frame through the RTL8139 (0x34, kind 0x04).  */
bool send_frame(const_byte_span frame);

/*  Asks the kernel to fill in the headers of a packet under construction
 *  (0x33) --- checksums included --- then sends it (0x36).  */
bool craft(PacketKind kind, byte_span packet);
bool send(PacketKind kind, byte_span packet);

} // namespace r2::net

#endif
