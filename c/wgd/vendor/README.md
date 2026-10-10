# Vendored WireGuard core

The separate CPU entropy fallback is documented in
[jitterentropy/R2.md](jitterentropy/R2.md), with its pinned revision and BSD
license. It is used only when RDSEED/RDRAND cannot supply randomness.

Source: https://github.com/smartalock/wireguard-lwip
Revision: `c54f20dbe76ac8b3411ad21e0ed7deea6f0cfd4d`
License: BSD 3-clause for the WireGuard core (retained in `wireguard/LICENSE`);
individual crypto files retain their original notices and X25519 license file.

Included: `wireguard.c`, `wireguard.h`, the platform declarations, `crypto.c`,
`crypto.h`, and the portable `crypto/refc` files. The lwIP interface and Cortex
assembly implementations are excluded. The r2 integration lives in `engine.c`
and `platform_r2.c`, rather than in a copy of `wireguardif.c`.

Local changes:

- Remove lwIP headers and unused netif/UDP PCB pointers. Address storage uses
  four IPv4 bytes, and ports use `uint16_t`. One peer and one allowed address.
- Fix the handshake rate-limit elapsed calculation to `now - last_rx`, allowing
  the first handshake and preserving unsigned millisecond wraparound.
- Use a 64-bit replay-window delta so a large counter jump cannot truncate
  into the existing window and admit a very old packet.
- Use explicit `uint32_t`/`uint64_t` arithmetic in Poly1305-donna-32. Its original
  `unsigned long` assumption does not hold on r2's LP64 amd64 target. Independent
  ChaCha20-Poly1305 vectors cover this change.
- Declare X25519's variable-length multiplier operand as a pointer rather than
  a fixed eight-limb array, avoiding an incorrect compiler overread diagnostic
  when the operand is the one-limb constant `a24`.

The freestanding standard C headers and memory/string functions in `compat/`
and `string.c` belong to the r2 adapter. Host tests use the host C library.
