#!/usr/bin/env python3
"""Test the real kernel timer assembly with a host-compatible return."""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
kernel = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else here.parents[4] / "r2_main"
source = (kernel / "src/abi/timer_interrupt.asm").read_text()
source = source.replace("timer_interrupt_stub:\n", "timer_interrupt_stub:\n    times 5 push qword 0\n")
source = source.replace("    iretq", "    add rsp, 40\n    ret")
source += "\nsection .note.GNU-stack noalloc noexec nowrite progbits\n"
with tempfile.TemporaryDirectory(prefix="streamd-fpu-") as temp:
    build = Path(temp)
    asm = build / "timer.asm"
    obj = build / "timer.o"
    exe = build / "context_test"
    asm.write_text(source)
    subprocess.run(["nasm", "-f", "elf64", str(asm), "-o", str(obj)], check=True)
    subprocess.run(["gcc", "-O2", "-mgeneral-regs-only", str(here / "context_test.c"), str(obj), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
