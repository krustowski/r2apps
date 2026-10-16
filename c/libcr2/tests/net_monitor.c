/* Real-kernel ABI smoke test. Run NETMON.ELF as the only network application
 * in an isolated QEMU VM with user networking (RTL8139 or E1000) and
 * -debugcon file:netmon.log. Success prints NETWORK ABI PASS. smoke.py
 * watches that marker and stops the VM; isa-debug-exit is optional.
 */
#include "syscall.h"

_Static_assert(sizeof(NetStats_T) == 40, "NetStats ABI");
_Static_assert(sizeof(NetPortBinding_T) == 32, "NetPortBinding ABI");
_Static_assert(sizeof(NetPortTable_T) == 528, "NetPortTable ABI");

static void debug(const char *text) {
    while (*text)
        write_port(0xe9, (uint8_t)*text++);
}

static void check(int ok, const char *message) {
    if (ok)
        return;
    debug("NETWORK ABI FAIL: ");
    debug(message);
    debug("\n");
    write_port(0xf4, 0x11);
    for (;;) {}
}

static int equal(const uint8_t *a, const uint8_t *b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int is_child(const char *s) {
    return s && s[0] == 'c' && s[1] == 'h' && s[2] == 'i' && s[3] == 'l' && s[4] == 'd' && !s[5];
}

int main(int argc, char **argv) {
    if (argc > 1 && is_child(argv[1])) {
        check(net_bind_port(45677) == 0, "child bind");
        sleep_ms(200);
        return 0; /* exit must release the binding */
    }

    struct { uint64_t before; NetStats_T value; uint64_t after; } stats = {0};
    struct { uint64_t before; NetPortTable_T value; uint64_t after; } ports = {0};
    stats.before = stats.after = ports.before = ports.after = 0x1122334455667788UL;
    check(get_net_stats(&stats.value) == 0, "get stats");
    check(get_net_ports(&ports.value) == 0, "get ports");
    check(stats.before == stats.after && ports.before == ports.after &&
          stats.after == 0x1122334455667788UL && ports.after == stats.after, "output bounds");
    check(ports.value.driver_pid == NET_NO_PID && ports.value.n_ports == 0, "empty registry");
    check(syscall(ScNetStats, 0, 0, 0) == 0xfc, "null stats pointer");
    check(syscall(ScNetPorts, 0, 0, 0) == 0xfc, "null ports pointer");
    check(syscall(ScNetStats, 0xa00000 - 39, 0, 0) == 0xfc, "stats cross-region pointer");
    check(syscall(ScNetPorts, 0xa00000 - 527, 0, 0) == 0xfc, "ports cross-region pointer");

    check(net_bind_port(45678) == 0 && net_bind_port(45679) == 0, "bind two ports");
    NetPortBinding_T owner;
    check(net_port_owner(45678, &owner) == 0, "lookup owner");
    uint64_t my_pid = owner.pid;
    check(owner.port == 45678 && owner.name[0] != 0, "owner identity");
    check(net_port_owner(45679, &owner) == 0 && owner.pid == my_pid, "same process owns both ports");
    check(net_unbind_port(45679) == 0 && net_port_owner(45679, &owner) == 0xfe, "unbind reflected");

    /* Two launches reuse a scheduler slot but have distinct process IDs. */
    uint8_t child_pid = 0;
    for (int round = 0; round < 2; round++) {
        check(run_elf((const uint8_t *)"NETMON", (const uint8_t *)"NETMON child", &child_pid) == 1, "launch child");
        sleep_ms(50);
        check(net_port_owner(45677, &owner) == 0 && owner.pid == child_pid, "process PID differs from reused slot");
        sleep_ms(250);
        check(net_port_owner(45677, &owner) == 0xfe, "exit releases child port");
    }

    check(net_register() == 0, "register NIC driver");
    check(get_net_ports(&ports.value) == 0 && ports.value.driver_pid == my_pid, "driver process identity");
    NetStatus_T ns;
    check(get_net_status(&ns) == 0, "legacy status preserved");
    SysInfo_T si = {0};
    si.ip_addr[0] = 10; si.ip_addr[2] = 2; si.ip_addr[3] = 15;
    check(write_sysinfo(&si), "publish local IP");

    uint8_t arp[42] = {0};
    for (int i = 0; i < 6; i++) {
        arp[i] = 255;
        arp[6 + i] = arp[22 + i] = ns.mac[i];
    }
    arp[12] = 8; arp[13] = 6;
    arp[15] = 1; arp[16] = 8; arp[18] = 6; arp[19] = 4; arp[21] = 1;
    for (int i = 0; i < 4; i++) arp[28 + i] = arp[38 + i] = si.ip_addr[i];
    arp[41] = 2; /* QEMU user-network gateway: 10.0.2.2 */
    NetStats_T before, after;
    check(get_net_stats(&before) == 0, "before TX sample");
    check(send_eth_frame(arp, sizeof(arp)) == 0, "send ARP");
    uint8_t frame[2048];
    int reply = 0;
    for (int tries = 0; tries < 200 && !reply; tries++) {
        int64_t len = receive_data_nb(4, frame);
        reply = len >= 42 && len <= 2048 && frame[12] == 8 && frame[13] == 6 && frame[21] == 2;
        if (!reply) sleep_ms(10);
    }
    check(reply, "receive gateway ARP reply");
    check(get_net_stats(&after) == 0 && after.tx_frames == before.tx_frames + 1 &&
          after.tx_bytes == before.tx_bytes + 60 && after.rx_frames > before.rx_frames &&
          after.rx_bytes >= before.rx_bytes + 42 && after.timestamp_ms >= before.timestamp_ms, "physical traffic counters");

    /* Local TCP is delivered through the registry without touching NIC totals. */
    uint8_t local[54] = {0};
    local[12] = 8; local[14] = 0x45; local[17] = 40; local[22] = 64; local[23] = 6;
    for (int i = 0; i < 4; i++) local[26 + i] = local[30 + i] = si.ip_addr[i];
    local[34] = 0x12; local[35] = 0x34; local[36] = 45678 >> 8; local[37] = 45678 & 255;
    local[46] = 0x50; local[47] = 2;
    before = after;
    check(send_eth_frame(local, sizeof(local)) == 0, "send loopback");
    /* Loopback replaces the Ethernet MACs with its device addresses. */
    check(receive_data_nb(4, frame) == sizeof(local) && equal(frame + 12, local + 12, sizeof(local) - 12), "receive loopback");
    check(get_net_stats(&after) == 0 && after.tx_frames == before.tx_frames && after.tx_bytes == before.tx_bytes, "loopback excludes physical TX");
    check(net_unbind_port(45678) == 0, "final unbind");
    debug("NETWORK ABI PASS\n");
    write_port(0xf4, 0x10);
    return 0;
}
