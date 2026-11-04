#!/usr/bin/env python3
"""Generates spec_example.nmc and prints the expected event log hash.

This encodes the example in section 7.4 of docs/SPEC.md by hand, in another language and without using
any project code. A test replays the file through nm_replay and compares against the printed hash, so
the C++ encoding, matching and hashing are all checked against an independent reading of the
specification.
"""

import pathlib
import struct

COMMAND_FORMAT = "<QQqIBBH"
EVENT_FORMAT = "<QQQQqIIIBBBB"
BUY, SELL = 1, 2
LIMIT = 1
ACCEPTED, TRADE, RESTED = 1, 3, 4


def header(magic, record_size):
    return struct.pack("<8sHHI", magic, 1, record_size, 0)


def fnv1a64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def main():
    commands = header(b"NMCMD\0\0\0", 32)
    commands += struct.pack(COMMAND_FORMAT, 1, 10, 100, 5, LIMIT, SELL, 0)
    commands += struct.pack(COMMAND_FORMAT, 2, 11, 101, 8, LIMIT, BUY, 0)

    # Fields: event_seq, command_seq, order_id, counter_order_id, price, quantity, leaves,
    # counter_leaves, type, side, reason, reserved.
    events = header(b"NMEVT\0\0\0", 56)
    for event in [
        (1, 1, 10, 0, 100, 5, 5, 0, ACCEPTED, SELL, 0, 0),
        (2, 1, 10, 0, 100, 5, 5, 0, RESTED, SELL, 0, 0),
        (3, 2, 11, 0, 101, 8, 8, 0, ACCEPTED, BUY, 0, 0),
        (4, 2, 11, 10, 100, 5, 3, 0, TRADE, BUY, 0, 0),
        (5, 2, 11, 0, 101, 3, 3, 0, RESTED, BUY, 0, 0),
    ]:
        events += struct.pack(EVENT_FORMAT, *event)

    output = pathlib.Path(__file__).with_name("spec_example.nmc")
    output.write_bytes(commands)
    print(f"commands=2 events=5 hash=0x{fnv1a64(events):016x}")


if __name__ == "__main__":
    main()
