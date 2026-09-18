/*
 *  net.cpp — the networking syscalls.
 */

#include "r2/io.hpp"
#include "r2/libc.hpp"
#include "r2/net.hpp"
#include "r2/time.hpp"

namespace r2::net {

uint16_t checksum(const_byte_span data) {
    uint32_t sum = 0;
    size_t len = data.size();
    const uint8_t *p = data.data();

    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];

    if (len & 1)
        sum += (uint32_t)p[len - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

string MacAddress::to_string() const {
    string result;
    StringWriter sink(result);

    for (int i = 0; i < 6; i++) {
        if (i)
            sink.put(':');
        format_uint(sink, octets[i], 16, 2, '0');
    }

    return result;
}

bool MacAddress::operator==(const MacAddress &other) const {
    return memcmp(octets, other.octets, sizeof(octets)) == 0;
}

string Ipv4::to_string() const {
    string result;
    StringWriter sink(result);

    for (int i = 0; i < 4; i++) {
        if (i)
            sink.put('.');
        format_uint(sink, octets[i]);
    }

    return result;
}

optional<Ipv4> Ipv4::parse(string_view text) {
    Ipv4 address;
    text = text.trim();

    for (int part = 0; part < 4; part++) {
        if (text.empty())
            return nullopt;

        size_t dot = text.find('.');
        string_view piece = (part == 3) ? text : text.substr(0, dot);

        if (part < 3 && dot == npos)
            return nullopt;
        if (piece.empty() || piece.size() > 3)
            return nullopt;

        uint64_t value = 0;
        if (!parse_uint(piece, value) || value > 255)
            return nullopt;

        address.octets[part] = (uint8_t)value;
        if (part < 3)
            text = text.substr(dot + 1);
    }

    return address;
}

optional<Status> status() {
    NetStatus raw;
    memset(&raw, 0, sizeof(raw));

    if (raw_syscall(Sys::NetStatus, (int64_t)&raw, 0) < 0)
        return nullopt;

    Status result;
    memcpy(result.mac.octets, raw.mac, sizeof(raw.mac));
    memcpy(result.ip.octets, raw.ip, sizeof(raw.ip));
    result.driver_active = raw.drv_active != 0;

    uint8_t count = raw.n_ports;
    if (count > 16)
        count = 16;

    if (result.bound_ports.reserve(count)) {
        for (uint8_t i = 0; i < count; i++)
            (void)result.bound_ports.push_back(raw.ports[i]);
    }

    return result;
}

optional<Ipv4> wait_for_address(uint64_t timeout_ms) {
    Stopwatch elapsed;

    for (;;) {
        auto current = status();
        if (current && !current->ip.is_unspecified())
            return current->ip;

        if (elapsed.elapsed_ms() >= timeout_ms)
            return nullopt;

        sleep(250);
    }
}

bool register_driver() { return raw_syscall(Sys::NetRegister, 0, 0) == 0; }

bool bind_port(uint16_t port) { return raw_syscall(Sys::NetRegister, port, 0) == 0; }

int64_t receive(PacketKind kind, byte_span buffer) {
    if (buffer.empty())
        return -1;
    return raw_syscall(Sys::ReceivePort, (int64_t)kind, (int64_t)buffer.data());
}

int64_t receive_nonblocking(byte_span buffer) {
    if (buffer.empty())
        return -1;
    /*  arg1 of 0 is what tells the kernel to return instead of blocking.  */
    return raw_syscall(Sys::ReceivePort, 0, (int64_t)buffer.data());
}

bool send_frame(const_byte_span frame) {
    if (frame.empty())
        return false;
    return raw_syscall(Sys::SendPacket, (int64_t)PacketKind::EthernetFrame,
                       (int64_t)frame.data(), (int64_t)frame.size()) == 0;
}

bool craft(PacketKind kind, byte_span packet) {
    if (packet.empty())
        return false;
    return raw_syscall(Sys::NewPacket, (int64_t)kind, (int64_t)packet.data()) == 0;
}

bool send(PacketKind kind, byte_span packet) {
    if (packet.empty())
        return false;
    return raw_syscall(Sys::SendPort, (int64_t)kind, (int64_t)packet.data()) == 0;
}

} // namespace r2::net
