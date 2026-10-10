# musl's libm, the part QuickJS needs

From musl 1.2.5 (<https://musl.libc.org/releases/musl-1.2.5.tar.gz>,
SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`),
under its [MIT license](COPYRIGHT): the double-precision functions behind
JavaScript's Math and Date (acos ... trunc, fmod, lrint, modf, pow, hypot,
cbrt and the rest), the kernels they share (`__sin`, `__cos`, `__rem_pio2`,
the exp/log/pow tables) and musl's x86-64 `sqrt`, `fabs` and `lrint`. The
sources in [`src/`](src/) are unmodified; [`include/`](include/) has the
three headers they need, written for this subset.

[`names.h`](names.h), force-included into every source and included by
libjsr2's `<math.h>`, renames each symbol to `jsr2m_*`: QuickJS gets these
correctly rounded functions and libc++r2 keeps its own `sin`, `cos` and
`floor` for the rest of the program. It was generated from `nm` over the
subset; adding a file means adding its symbols there.

Built by [`../../libjsr2/Makefile`](../../libjsr2/Makefile) with musl's own
`-fexcess-precision=standard -frounding-math`. About 38 KB of code.
