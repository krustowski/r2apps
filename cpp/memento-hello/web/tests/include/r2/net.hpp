#pragma once
#include "types.hpp"
#include <optional>
namespace r2::net {
struct Ipv4 {
    uint8_t octets[4];
    bool is_unspecified() const { return !(octets[0] | octets[1] | octets[2] | octets[3]); }
};
struct MacAddress { uint8_t octets[6]; };
struct Status { MacAddress mac; Ipv4 ip; };
struct Config {
    Ipv4 ip, netmask, gateway, dns;
    MacAddress mac, gateway_mac;
    bool gateway_mac_known;
};
std::optional<Status> status();
std::optional<Config> config();
bool bind_port(uint16_t port);
}
