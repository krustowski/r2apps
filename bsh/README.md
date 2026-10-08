# bsh scripts

Example scripts for [bsh](../c/bsh/README.md), the base shell of `r2sh` and `tnt`: a command a line,
`#` for comments, `$0` for the script and `$1` to `$9` for what follows its name (`$$` is a `$`).
r2_main's `make build_iso` puts them in `/opt/bsh` on the CD.

| Script | What it does |
|--------|--------------|
| `HELLO.BSH` | a greeting, and `sysinfo` |
| `SYSTEM.BSH` | the machine at a glance: `mount`, `ts`, `meminfo` |
| `TMPDEMO.BSH` | a directory on the RAM disk, made, listed and removed |
| `COUNT.BSH` | a countdown with `sleep` |
| `ARGS.BSH` | how arguments come in: `bsh ARGS.BSH /mnt/tar opt` |

Run one in r2sh or tnt with `bsh /mnt/iso/opt/bsh/HELLO.BSH`, start r2sh with it
(`sh --run /mnt/iso/opt/bsh/HELLO.BSH`), or pick *Run in shell* on it in Memento's file browser,
which opens a Shell window that runs it and stays open.
