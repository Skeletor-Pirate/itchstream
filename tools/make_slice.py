#!/usr/bin/env python3
"""Copy whole ITCH messages from stdin to a file until a byte budget is reached.

Why not `head -c`: cutting at an arbitrary byte leaves a half message at the end
of the slice, which the parser correctly reports as a truncated tail. Slicing on
a message boundary keeps every slice a valid file, so a benchmark's message
count and the exit code both mean something.

    zcat day.ITCH50.gz | ./tools/make_slice.py 300000000 > slice.itch
    ./tools/make_slice.py --messages 5000000 < day.ITCH50 > small.itch
"""

from __future__ import annotations

import argparse
import sys


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bytes", nargs="?", type=int, default=300_000_000, help="byte budget")
    parser.add_argument("--messages", type=int, default=0, help="stop after N messages instead")
    args = parser.parse_args(argv)

    source = sys.stdin.buffer
    sink = sys.stdout.buffer

    written = 0
    messages = 0

    while True:
        if args.messages:
            if messages >= args.messages:
                break
        elif written >= args.bytes:
            break

        header = source.read(2)
        if len(header) < 2:
            break  # clean end of input, or one stray byte that we drop

        length = int.from_bytes(header, "big")
        if length == 0:
            raise SystemExit("make_slice: zero length prefix in input")

        body = source.read(length)
        if len(body) < length:
            # The source was cut mid-message (or the download failed): drop the
            # partial message and keep everything before it.
            print(
                f"make_slice: input ended mid-message at {written} bytes; "
                f"wrote the {messages} complete messages before it",
                file=sys.stderr,
            )
            break

        sink.write(header)
        sink.write(body)
        written += 2 + length
        messages += 1

    sink.flush()
    print(f"make_slice: {messages} messages, {written} bytes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
