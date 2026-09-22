# dish

[dish](https://github.com/thevxn/dish) --- the tiny one-shot monitoring
service --- carried over to `r2`.

It reads a list of sockets, checks every one of them over HTTP, TCP or ICMP,
and reports the results to whichever channels are configured.  The socket
schema, the flags, the results payload and the exit codes are upstream's, so
the same `sockets.json` and the same remote API serve a dish on Linux and a
dish on `r2`.

What is different is underneath.  There is no `net` package here, no socket
layer in the kernel and no TLS, so the checks run on
[`r2net`](../r2net/README.md), this repository's own TCP/IP stack, and the two
alerting channels that are HTTPS-only cannot be reached at all.

```
12:56:21 [ INFO  ]: dish run: started
12:56:21 [ DEBUG ]: network: link eth, address 10.0.2.15, gateway 10.0.2.2, icmp true
12:56:21 [ DEBUG ]: ICMP runner: reply from 10.0.2.2 in 18ms
12:56:21 [ INFO  ]: results pushed to remote API
12:56:21 [ WARN  ]: dish run: some tests failed:
* 10.0.2.2:0 -- success [ OK ]
* http://10.0.2.2:8080/health -- success [ OK ]
* 10.0.2.2:8080 -- success [ OK ]
* 10.0.2.2:9099 -- failed [FAIL] -- connection refused
```

## Build and run

```shell
cd ../tinygo-r2 && make image    # once
cd ../dish && make               # produces dish.elf
```

Put the binary, a socket list and a configuration file on the floppy:

```shell
mcopy -i fat.img dish/dish.elf              ::BIN/DISH.ELF
mcopy -i fat.img dish/configs/demo_sockets.json ::DISH/SOCKETS.JSN
mcopy -i fat.img dish/configs/dish.cfg      ::DISH/DISH.CFG
```

and run it from the shell, or from `INIT.RC` as a boot-time check:

```
fg DISH -config /mnt/fat/DISH/DISH.CFG
```

## Eight arguments

The kernel's ELF loader passes a program **at most eight argv tokens**, the
program name included, and says nothing when it drops the rest.  A `-flag
value` pair costs two of them, so a dish run with a source, a network and two
alert channels is over the limit before it starts.

Hence `-config`, which reads the same settings from a file, one `key = value`
per line, with `#` for comments --- the arrangement and the format `garn`
uses.  Keys are flag names.  A flag given on the command line overrides the
file, so the file can hold the settings that never change:

```
fg DISH -config /mnt/fat/DISH/DISH.CFG -verbose
```

dish warns when a run used all eight tokens, since that is the only way to
notice that a flag went missing.

## Source

The list of sockets comes from a file on a mounted filesystem or from an
`http://` URL, exactly as upstream, and the JSON is upstream's schema:

```json
{
  "sockets": [
    {
      "id": "garn_http",
      "socket_name": "garn HTTP",
      "host_name": "http://10.3.4.2",
      "port_tcp": 80,
      "path_http": "/info",
      "expected_http_code_array": [200]
    }
  ]
}
```

The protocol for each socket is chosen by upstream's rules, first match wins:

+ `host_name` starting with `http://` --- **HTTP**, and the status has to be
  one of `expected_http_code_array`;
+ `port_tcp` between 1 and 65535 --- **TCP**, and the check passes if the
  connection opens;
+ `host_name` not empty --- **ICMP** echo;
+ otherwise the check fails.

A socket list can be given as a positional argument (`fg DISH
/mnt/fat/DISH/SOCKETS.JSN`) or, when the tokens are needed elsewhere, as
`source` in the configuration file.

## Flags

Upstream's flags, unchanged:

```
-name string          dish instance name (default "generic-dish")
-timeout uint         timeout in seconds for http and tcp calls (default 10)
-verbose              console logging toggle
-textNotifySuccess    report successful runs to text channels
-machineNotifySuccess report successful runs to machine channels
-hname string         name of a custom header for the remote API
-hvalue string        value of that header
-target string        Pushgateway URL
-updateURL string     API endpoint URL for pushing results
-webhookURL string    webhook endpoint URL
```

and the ones this machine needs:

```
-config string        file holding these same settings, one `key = value` per line
-source string        the socket list, for when there is no room for a positional argument
-net string           link to run the checks over: eth or slip (default "eth")
-ip string            this machine's IPv4 address (default: from the kernel, else 10.3.4.2)
-mask string          what counts as the local link (default 255.255.255.0)
-gw string            gateway for everything else (default: the .1 of the local network)
-dns string           DNS server for host names (default: none, addresses only)
-serialLog            mirror the log to COM1 (eth only)
-netDebug             trace every packet sent and received
```

`-telegramBotToken`, `-telegramChatID`, `-discordBotToken`, `-discordChannelId`,
`-cache`, `-cacheDir` and `-cacheTTL` are still accepted so that a command line
written for upstream dish starts here, but each one produces a warning and is
ignored.  See below.

## Exit codes

Upstream's, so a script that switches on them does not have to know which
machine ran the check:

| Code | Meaning |
|------|---------|
| 0 | All checks passed |
| 1 | No socket source provided |
| 2 | Failed to parse command-line arguments |
| 3 | Failed to run tests on sockets |
| 4 | One or more sockets could not be reached |

## What does not work here, and why

**No TLS, so no `https://` and no Telegram or Discord.**  There is no entropy
source on this machine, no big-integer arithmetic worth the name and no room
for a certificate chain in a 2 MiB frame.  An `https://` socket fails its check
with a message saying so rather than being quietly downgraded, and the two
HTTPS-only alerting channels are refused at startup.  A dish on `r2` that must
reach Telegram should push to a webhook on a host that can --- one line of
relay, and the bot token stays off the floppy.

**No caching of the socket list.**  Upstream caches what it fetched from the
remote API and falls back to it when the API is down, keyed on the file's
modification time.  The ABI has no writable path that carries one.  `-cache` is
accepted and ignored; the source is fetched on every run.

**The checks run one after another, not concurrently.**  Upstream starts a
goroutine per socket.  Here the whole TCP/IP stack is one cooperatively
scheduled object in this process, a goroutine commits 32 KiB of stack before it
runs once, and the kernel delivers frames one at a time through a single shared
buffer.  A list where several hosts are down takes as long as the sum of their
timeouts.

**ICMP and DNS need the Ethernet driver registration.**  The kernel routes
frames to a process by TCP destination port and hands everything else --- ARP,
ICMP, UDP --- to whichever process registered as the global Ethernet driver.
dish takes that role when nobody else has it, and then answers ARP and ping for
the whole machine while it runs.  When `eth.elf` or `garn` is already running,
dish binds the TCP ports it needs instead: HTTP and TCP checks work, ICMP
checks and name resolution report that they cannot.  The run says which of the
two it got:

```
[ WARN  ]: another process holds the Ethernet driver: ICMP checks and name resolution are unavailable
```

The registration is never released, not even when the process exits, so the
first dish run after a boot leaves the machine without an ARP responder when it
finishes.  Running `eth.elf` first avoids that, at the cost of ICMP checks.

**No colours, and no emoji in the report.**  The kernel's console writes bytes
into a VGA text buffer in code page 437: ANSI escapes appear as literal
rubbish, and a multi-byte character arrives as pieces of line drawing.  The
same two states are said in ASCII.

**Host names need `-dns`.**  There is no resolver on this machine and no
`/etc/resolv.conf` to read one out of.  Addresses in the socket list always
work; names work when a server is given.

## Reading a headless run

With no screen attached, `-serialLog` mirrors the log to COM1, which is where
QEMU's `-serial stdio` picks it up.  It is not available with `-net slip`,
where COM1 is the network.  The kernel's own boot messages go out on the same
port, so a host-side SLIP peer sees them as junk before the first real frame.

## How this was verified

On the real kernel under QEMU, not just compiled:

+ **Ethernet**, with `-netdev user -device rtl8139`: ICMP echo, a TCP connect
  check, an HTTP check against a server on the host, a check against a closed
  port failing with `connection refused`, and all three machine channels
  receiving upstream's exact payloads ---
  `{"dish_results":{"closed_tcp":false,...}}` to the API and the webhook,
  `dish_failed_count 1` to Pushgateway under
  `/metrics/job/dish_results/instance/r2-dish`.
+ **SLIP**, against a host-side SLIP peer on a QEMU serial socket: an ICMP
  check over `192.168.7.2 -> 192.168.7.1`, and `dish run: all tests ok`.
+ Name resolution against a real resolver through the QEMU gateway.
+ `encoding/json`, `flag` and the FAT12 file reads that the configuration
  depends on, all of which work on this target.

A four-check run with three alert pushes --- seven TCP connections --- finishes
in under a second.
