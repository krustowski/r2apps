"""The kernel maps a private 2 MiB image; shared-heap RAM cannot enlarge it."""
import subprocess
import sys
from pathlib import Path
name = Path(sys.argv[1]).stem
symbols = subprocess.check_output(["nm", "-n", sys.argv[1]], text=True)
end = next(int(line.split()[0], 16) for line in symbols.splitlines()
           if line.split()[-1] == "__r2_image_end")
if end > 0x800000:
    sys.exit(f"{name} image exceeds its 2 MiB frame: end=0x{end:x}")
print(f"{name} image incl. stack: {end-0x600000} bytes; {0x800000-end} bytes spare")
