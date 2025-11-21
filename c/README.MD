# C library (libcr2) and apps

| Project name | Purpose | State |
| ------------ | ------- | ----- |
| `garn` | Sample HTTP server implementation. | unsable |
| `hello-fs` | Example with filesystem-related operations. | stable |
| `hello-world` | Example to show minimal C program crafted for use with the `r2` kernel. | stable |
| `icmpresp` | ICMP responder. Sample ICMP Echo service to demonstrate portions of implemented networking. | stable |
| `r2sh` | Custom userland shell for `r2`. | unstable |
| `them` | A simple 16-bit CPU emulator focused on MS-DOS. | unstable |

## crt0

~~There is no C runtime version 0 (crt0) available yet. That's why the linker script is required to link compiled programs into ELF executables and to skip crt0. *This is a technical debt*~~

C runtime version 0 (crt0) is now available to modify in `libcr2/_crt0.asm`.

## libcr2 

`libcr2` is a custom C library dedicated for use with the `r2` kernel. Mainly, it is built on the kernel syscalls implementation to provide basic functions like `sys_print`, etc.

To produce a statically linkable library/archive (`libcr2.a`), run:

```shell
make libcr2
```
