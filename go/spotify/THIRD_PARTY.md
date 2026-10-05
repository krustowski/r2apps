# Streaming dependencies

The native application has no host streaming dependency.

- `third_party/shannon`: [devgianlu/shannon](https://github.com/devgianlu/shannon), MIT. Exact commit is recorded in `UPSTREAM`; license is included in `LICENSE`. Tests use a small standard-library assertion helper instead of testify. Local changes iterate words directly and pass word values through callbacks, avoiding per-word slice tables and allocations during packet encryption/decryption.
- `codec/vendor/tremor`: [Xiph Tremor](https://gitlab.xiph.org/xiph/tremor), BSD 3-clause. Exact commit and license are included in `UPSTREAM` and `COPYING`. Only decoding sources are included. Header include paths are adapted to the vendored layout, and allocation calls are redirected through our freestanding libc headers. A local integer heapsort replaces libc qsort. Codebook setup uses checked heap scratch and frees it on failure instead of allocating large tables on the goroutine stack.
- `codec/vendor/ogg`: [Xiph libogg](https://github.com/xiph/ogg), BSD 3-clause. Exact commit and license are included in `UPSTREAM` and `COPYING`. Includes use the vendored layout; fixed-width config types are provided for x86_64 r2.
- `codec/testdata/tone.ogg`: a generated 330 Hz test tone, released under CC0; no Spotify audio is bundled.

The Go code in `stream` implements the wire protocol directly. Public message
schemas, application identifiers and protocol trust anchors were checked against
[go-librespot](https://github.com/devgianlu/go-librespot). Its GPL implementation
is not linked or vendored. These private Spotify endpoints can change and are
not a supported public playback API.
