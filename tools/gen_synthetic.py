#!/usr/bin/env python3
"""Generate a structurally valid NASDAQ TotalView-ITCH 5.0 file.

Two uses:

  * tests and demos need a file that is not NASDAQ's (their sample files are
    multi-gigabyte and must not be committed), and
  * the parser benchmark needs a large input before the real slice exists.

Every message is emitted with the 2-byte big-endian length prefix that the
NASDAQ sample files use, in specification field order, so the output is
accepted by the C++ walker and by the golden-reference script.

This is synthetic data. Numbers measured on it are labelled synthetic: the
message mix is realistic-ish, not real, and it says nothing about real feed
behaviour.

    ./tools/gen_synthetic.py -o data/synthetic.itch --messages 1000000
    ./tools/gen_synthetic.py -o data/tiny.itch --messages 200 --seed 7
"""

from __future__ import annotations

import argparse
import json
import random
import sys

# Wire sizes from the ITCH 5.0 specification. The C++ side asserts the same
# numbers; the verifier at the bottom of this file re-checks every generated
# message against this table.
WIRE_SIZES = {
    "S": 12,
    "R": 39,
    "A": 36,
    "F": 40,
    "E": 31,
    "C": 36,
    "X": 23,
    "D": 19,
    "U": 35,
    "P": 44,
    "H": 25,  # Stock Trading Action: skipped by ItchStream, emitted as noise
}

SYMBOLS = ("AAPL", "MSFT", "SPY", "QQQQ", "TSLA")

# 4 implied decimals, so 1_800_000 is 180.0000.
BASE_PRICES = {
    "AAPL": 18_000_000,
    "MSFT": 42_000_000,
    "SPY": 55_000_000,
    "QQQQ": 38_000_000,
    "TSLA": 25_000_000,
}

TICK = 100  # one cent at 4 decimals
MARKET_OPEN_NS = 34_200 * 1_000_000_000  # 09:30:00


def be(value: int, width: int) -> bytes:
    return int(value).to_bytes(width, "big", signed=False)


def text(value: str, width: int) -> bytes:
    return value.encode("ascii")[:width].ljust(width, b" ")


def system_event(locate: int, tracking: int, ts: int, code: str) -> bytes:
    return b"S" + be(locate, 2) + be(tracking, 2) + be(ts, 6) + code.encode("ascii")


def stock_directory(locate: int, tracking: int, ts: int, symbol: str) -> bytes:
    return (
        b"R"
        + be(locate, 2)
        + be(tracking, 2)
        + be(ts, 6)
        + text(symbol, 8)  # stock
        + b"Q"  # market category
        + b"N"  # financial status indicator
        + be(100, 4)  # round lot size
        + b"N"  # round lots only
        + b"C"  # issue classification
        + b"  "  # issue sub-type
        + b"P"  # authenticity
        + b"N"  # short sale threshold indicator
        + b"N"  # IPO flag
        + b"1"  # LULD reference price tier
        + b"N"  # ETP flag
        + be(0, 4)  # ETP leverage factor
        + b"N"  # inverse indicator
    )


def add_order(
    locate: int,
    tracking: int,
    ts: int,
    ref: int,
    side: str,
    shares: int,
    symbol: str,
    price: int,
    mpid: str | None = None,
) -> bytes:
    body = (
        be(locate, 2)
        + be(tracking, 2)
        + be(ts, 6)
        + be(ref, 8)
        + side.encode("ascii")
        + be(shares, 4)
        + text(symbol, 8)
        + be(price, 4)
    )
    if mpid is None:
        return b"A" + body
    return b"F" + body + text(mpid, 4)


def order_executed(locate: int, tracking: int, ts: int, ref: int, shares: int, match: int) -> bytes:
    return b"E" + be(locate, 2) + be(tracking, 2) + be(ts, 6) + be(ref, 8) + be(shares, 4) + be(match, 8)


def order_executed_with_price(
    locate: int, tracking: int, ts: int, ref: int, shares: int, match: int, price: int
) -> bytes:
    return (
        b"C"
        + be(locate, 2)
        + be(tracking, 2)
        + be(ts, 6)
        + be(ref, 8)
        + be(shares, 4)
        + be(match, 8)
        + b"Y"
        + be(price, 4)
    )


def order_cancel(locate: int, tracking: int, ts: int, ref: int, shares: int) -> bytes:
    return b"X" + be(locate, 2) + be(tracking, 2) + be(ts, 6) + be(ref, 8) + be(shares, 4)


def order_delete(locate: int, tracking: int, ts: int, ref: int) -> bytes:
    return b"D" + be(locate, 2) + be(tracking, 2) + be(ts, 6) + be(ref, 8)


def order_replace(
    locate: int, tracking: int, ts: int, original: int, new: int, shares: int, price: int
) -> bytes:
    return (
        b"U"
        + be(locate, 2)
        + be(tracking, 2)
        + be(ts, 6)
        + be(original, 8)
        + be(new, 8)
        + be(shares, 4)
        + be(price, 4)
    )


def trade(
    locate: int, tracking: int, ts: int, ref: int, side: str, shares: int, symbol: str, price: int, match: int
) -> bytes:
    return (
        b"P"
        + be(locate, 2)
        + be(tracking, 2)
        + be(ts, 6)
        + be(ref, 8)
        + side.encode("ascii")
        + be(shares, 4)
        + text(symbol, 8)
        + be(price, 4)
        + be(match, 8)
    )


def operational_halt(locate: int, tracking: int, ts: int, symbol: str, state: str) -> bytes:
    # 'H' is 25 bytes: type(1) locate(2) tracking(2) ts(6) stock(8) state(1)
    # reserved(1) reason(4).
    return (
        b"H"
        + be(locate, 2)
        + be(tracking, 2)
        + be(ts, 6)
        + text(symbol, 8)
        + state.encode("ascii")
        + b" "
        + b"    "
    )


class Emitter:
    def __init__(self, out) -> None:
        self.out = out
        self.bytes_written = 0
        self.messages = 0
        self.counts: dict[str, int] = {}

    def emit(self, body: bytes) -> None:
        self.out.write(be(len(body), 2))
        self.out.write(body)
        self.bytes_written += 2 + len(body)
        self.messages += 1
        key = chr(body[0])
        self.counts[key] = self.counts.get(key, 0) + 1


def generate(out, messages: int, seed: int) -> dict:
    rng = random.Random(seed)
    emitter = Emitter(out)

    locate = {symbol: index + 1 for index, symbol in enumerate(SYMBOLS)}
    live: dict[str, list[dict]] = {symbol: [] for symbol in SYMBOLS}
    next_ref = 1
    match_number = 1
    tracking = 0
    ts = MARKET_OPEN_NS

    def stamp() -> int:
        nonlocal ts
        ts += rng.randrange(1, 50_000)  # 1..50 microseconds later
        return ts

    def take_ref() -> int:
        nonlocal next_ref
        ref = next_ref
        next_ref += 1
        return ref

    emitter.emit(system_event(0, 0, stamp(), "O"))
    for symbol in SYMBOLS:
        tracking += 1
        emitter.emit(stock_directory(locate[symbol], tracking, stamp(), symbol))

    while emitter.messages < messages:
        symbol = rng.choice(SYMBOLS)
        loc = locate[symbol]
        tracking += 1
        roll = rng.random()
        orders = live[symbol]

        if not orders or roll < 0.70:
            side = "B" if rng.random() < 0.5 else "S"
            offset = rng.randrange(0, 50) * TICK
            price = BASE_PRICES[symbol] - offset if side == "B" else BASE_PRICES[symbol] + offset
            shares = 100 * rng.randrange(1, 20)
            ref = take_ref()
            mpid = "NSDQ" if rng.random() < 0.1 else None
            emitter.emit(add_order(loc, tracking, stamp(), ref, side, shares, symbol, price, mpid))
            orders.append({"ref": ref, "side": side, "price": price, "shares": shares})

        elif roll < 0.80:
            order = rng.choice(orders)
            executed = order["shares"] if rng.random() < 0.4 else rng.randrange(1, order["shares"] + 1)
            match_number += 1
            if rng.random() < 0.2:
                emitter.emit(
                    order_executed_with_price(
                        loc, tracking, stamp(), order["ref"], executed, match_number, order["price"]
                    )
                )
            else:
                emitter.emit(
                    order_executed(loc, tracking, stamp(), order["ref"], executed, match_number)
                )
            order["shares"] -= executed
            if order["shares"] == 0:
                orders.remove(order)

        elif roll < 0.88:
            order = rng.choice(orders)
            cancelled = rng.randrange(1, order["shares"] + 1)
            emitter.emit(order_cancel(loc, tracking, stamp(), order["ref"], cancelled))
            order["shares"] -= cancelled
            if order["shares"] == 0:
                orders.remove(order)

        elif roll < 0.94:
            order = rng.choice(orders)
            emitter.emit(order_delete(loc, tracking, stamp(), order["ref"]))
            orders.remove(order)

        elif roll < 0.98:
            order = rng.choice(orders)
            new_ref = take_ref()
            shares = 100 * rng.randrange(1, 20)
            price = order["price"] + (TICK if order["side"] == "B" else -TICK)
            emitter.emit(order_replace(loc, tracking, stamp(), order["ref"], new_ref, shares, price))
            orders.remove(order)
            orders.append({"ref": new_ref, "side": order["side"], "price": price, "shares": shares})

        else:
            order = rng.choice(orders)
            shares = rng.randrange(1, order["shares"] + 1)
            match_number += 1
            emitter.emit(
                trade(loc, tracking, stamp(), order["ref"], order["side"], shares, symbol,
                      order["price"], match_number)
            )

        # One percent noise: a type this version skips, so the walker's
        # skip-by-length path is exercised by every generated file.
        if rng.random() < 0.01:
            tracking += 1
            emitter.emit(operational_halt(loc, tracking, stamp(), symbol, "T"))

    emitter.emit(system_event(0, 0, stamp(), "C"))

    return {
        "messages": emitter.messages,
        "bytes": emitter.bytes_written,
        "by_type": dict(sorted(emitter.counts.items())),
        "symbols": list(SYMBOLS),
        "seed": seed,
    }


def verify(path: str) -> dict:
    """Re-read the generated file and check every length prefix and type size."""
    counts: dict[str, int] = {}
    messages = 0
    with open(path, "rb") as handle:
        data = handle.read()

    offset = 0
    while offset < len(data):
        if offset + 2 > len(data):
            raise SystemExit(f"verify: stray byte at offset {offset}")
        length = int.from_bytes(data[offset : offset + 2], "big")
        body = data[offset + 2 : offset + 2 + length]
        if len(body) != length:
            raise SystemExit(f"verify: message at offset {offset} claims {length} bytes, has {len(body)}")
        kind = chr(body[0])
        if length == 0:
            raise SystemExit(f"verify: zero length at offset {offset}")
        if kind in WIRE_SIZES and WIRE_SIZES[kind] != length:
            raise SystemExit(
                f"verify: type '{kind}' at offset {offset} is {length} bytes, spec says {WIRE_SIZES[kind]}"
            )
        counts[kind] = counts.get(kind, 0) + 1
        messages += 1
        offset += 2 + length

    return {"messages": messages, "bytes": len(data), "by_type": dict(sorted(counts.items()))}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Generate a synthetic ITCH 5.0 file")
    parser.add_argument("-o", "--output", help="output file (default: stdout)")
    parser.add_argument("--messages", type=int, default=1000, help="approximate message count")
    parser.add_argument("--seed", type=int, default=1, help="RNG seed for reproducible files")
    parser.add_argument("--no-verify", action="store_true", help="skip the re-read verification pass")
    parser.add_argument("--quiet", action="store_true", help="do not print the summary")
    args = parser.parse_args(argv)

    if args.messages < 1:
        parser.error("--messages must be at least 1")

    if args.output:
        with open(args.output, "wb") as handle:
            summary = generate(handle, args.messages, args.seed)
        if not args.no_verify:
            checked = verify(args.output)
            if checked["messages"] != summary["messages"] or checked["bytes"] != summary["bytes"]:
                raise SystemExit(
                    f"verify: wrote {summary['messages']} messages / {summary['bytes']} bytes, "
                    f"re-read {checked['messages']} / {checked['bytes']}"
                )
            summary["verified"] = True
    else:
        if sys.stdout.isatty():
            parser.error("refusing to write binary data to a terminal; use -o FILE")
        summary = generate(sys.stdout.buffer, args.messages, args.seed)
        summary["verified"] = False

    if not args.quiet:
        print(json.dumps(summary, indent=2), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
