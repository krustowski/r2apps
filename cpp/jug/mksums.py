#!/usr/bin/env python3
"""Publish the SHA-256, byte size and relative path of r2 ELF programs."""

import argparse
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile

NAME = re.compile(r"[a-zA-Z0-9_-]{1,8}\Z")
PATH = re.compile(r"[a-zA-Z0-9_./-]{1,63}\Z")


def manifest(root: Path, updated: str) -> str:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError(f"not a directory: {root}")
    programs = {}
    for file in sorted(root.rglob("*")):
        if file.suffix.lower() != ".elf" or not file.is_file():
            continue
        rel = file.relative_to(root).as_posix()
        if not NAME.fullmatch(file.stem) or not PATH.fullmatch(rel):
            raise ValueError(f"not an r2 program name/path: {rel}")
        try:
            file.resolve(strict=True).relative_to(root)
        except ValueError:
            raise ValueError(f"program points outside the repository: {rel}") from None
        name = file.stem.lower()
        if name in programs:
            raise ValueError(f"duplicate program {name}: {programs[name][2]} and {rel}")
        digest = hashlib.sha256()
        size = 0
        with file.open("rb") as stream:
            while chunk := stream.read(64 * 1024):
                size += len(chunk)
                digest.update(chunk)
        if size > 0xFFFFFFFF:
            raise ValueError(f"program is larger than r2 can store: {rel}")
        programs[name] = (digest.hexdigest(), size, rel)
    lines = [f"# updated {updated}", "# SHA-256  bytes  path (relative to this list)"]
    lines += [f"{digest}  {size}  {rel}" for digest, size, rel in
              (programs[name] for name in sorted(programs))]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path, help="replace this list after generation succeeds")
    parser.add_argument("--updated", help="list timestamp; defaults to the current UTC time")
    args = parser.parse_args()
    temporary = None
    try:
        updated = args.updated or datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
        if "\n" in updated or "\r" in updated or len(updated) >= 48 or not updated:
            raise ValueError("timestamp must fit on one line of fewer than 48 characters")
        text = manifest(args.directory, updated)
        if args.output:
            with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", dir=args.output.parent,
                                             prefix=".sums-", delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(text)
            os.chmod(temporary, 0o644)
            os.replace(temporary, args.output)
            temporary = None
        else:
            sys.stdout.write(text)
        return 0
    except (OSError, ValueError) as error:
        print(f"mksums: {error}", file=sys.stderr)
        return 1
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
