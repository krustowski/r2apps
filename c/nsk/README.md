# nsk

The networking swiss knife. Sweeps an IPv4 subnet given in CIDR notation and lists the live computers found in it.

| Argmuent | Pair value (if any) | Meaning | Required |
|----------|---------------------|---------|----------|
| *(positional)* | CIDR, e.g. `10.3.4.0/30` | The subnet to scan. A bare address means `/32`. | Yes |
| `--arp`   | *none*        | ARP sweep only, performed even when the range is off-link. | No |
| `--icmp`  | *none*        | ICMP echo sweep only, and every address is pinged. | No |
| `--timeout` | milliseconds | How long to listen for replies after each sweep. | No, default is `1000`. |
| `--ip`    | IPv4 address  | Overrides the local IPv4 address (otherwise read from sysinfo). | No |
| `debug`   | *none*        | Starts the verbose mode. | No |
| `--help`, `-h` | *none*   | Prints the usage. | No |

## How it works

1. The ARP sweep broadcasts a who-has request for every address in the range. Anything that answers is alive and its MAC is recorded. ARP is skipped when the range is off-link (see below).
2. The ICMP sweep then pings the addresses that stayed silent, unicast when the MAC is already known and broadcast otherwise. With `--icmp` every address is pinged instead.
3. Both sweeps wait `--timeout` milliseconds for late replies, so a scan takes about two timeouts.

Round trip times are reported at the 10 ms resolution of the kernel PIT tick.

The network and broadcast addresses are skipped for `/30` and wider prefixes; `/31` and `/32` ranges are scanned in full. At most 1024 addresses are scanned per run, so `/22` is the widest usable prefix.

ARP only reaches the local link. Since r2 publishes no netmask, the range counts as local when it contains the local address or when the whole range shares its `/24`; `--arp` forces the sweep anyway.

## Requirements

nsk drives the Ethernet NIC directly — the SLIP driver has no ARP and is not supported.

Replies are delivered by the kernel to the process registered as the global Ethernet driver, so nsk claims that role for the scan (syscall `0x37`). The kernel releases that registration when nsk exits (or is killed, or crashes), so scans can be repeated. The first registrant wins, which means **`eth.elf` must not be running at the same time**; nsk prints a warning and finds nothing if it is. Kill `eth` first (`ts`, then `kill <id>`) and the next scan takes the NIC over. While nsk holds the NIC it answers ARP who-has requests and ICMP echo requests addressed to the local IP itself, so the machine stays reachable during a scan.

Frames sent from the local MAC are ignored, otherwise the broadcast probes looping back through the host bridge would be counted as live hosts. Other r2 processes on the same guest share that MAC, so they are not discoverable by a scan.

## Example

```sh
fg NSK 10.3.4.0/30
```

```
-> nsk: scanning 10.3.4.0/30 (2 addresses)
-> local 10.3.4.2  52:54:00:12:34:56
>> arp sweep
<< 10.3.4.1 is up  52:54:00:12:35:01  (arp, 10 ms)
>> icmp sweep

-> live hosts:
   10.3.4.1         52:54:00:12:35:01  arp  10 ms
   10.3.4.2         52:54:00:12:34:56  self

-> 2 up / 2 scanned in 2010 ms
```

```sh
fg NSK 10.3.4.0/24 --timeout 500 debug
```

nsk prints a report and exits, so `fg` suits it better than `bg`. The CIDR is required — `fg NSK` on its own only prints the usage.
