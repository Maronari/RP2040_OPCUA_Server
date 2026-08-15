#!/usr/bin/env python3
"""Bounded hardware check for lwIP OPC UA listener capacity recovery."""

from __future__ import annotations

import argparse
import socket
import struct
import threading
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Saturate TCP accept capacity, release it, and require listener recovery."
    )
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=4840)
    parser.add_argument("--connections", type=int, default=24)
    parser.add_argument("--connect-timeout", type=float, default=0.5)
    parser.add_argument("--recovery-timeout", type=float, default=20.0)
    parser.add_argument("--serial-port", default="COM6")
    parser.add_argument("--serial-baud", type=int, default=115200)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.connections < 1 or args.connections > 64:
        raise SystemExit("listener-recovery: FAIL: --connections must be 1..64")
    if args.connect_timeout <= 0 or args.recovery_timeout <= 0:
        raise SystemExit("listener-recovery: FAIL: timeouts must be positive")

    try:
        import serial
    except ImportError as exc:
        raise SystemExit("listener-recovery: FAIL: pyserial is required") from exc

    captured = bytearray()
    stop_reader = threading.Event()
    serial_port = serial.Serial(args.serial_port, args.serial_baud, timeout=0.1)
    serial_port.reset_input_buffer()

    def read_serial() -> None:
        while not stop_reader.is_set():
            captured.extend(serial_port.read(4096))

    reader = threading.Thread(target=read_serial, daemon=True)
    reader.start()
    sockets: list[socket.socket] = []
    accepted_count = 0
    attempted = args.connections
    try:
        for _ in range(attempted):
            candidate = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            candidate.settimeout(args.connect_timeout)
            try:
                candidate.connect((args.host, args.port))
            except OSError:
                candidate.close()
                continue
            sockets.append(candidate)

        if len(sockets) < 2:
            raise SystemExit(
                f"listener-recovery: FAIL: only {len(sockets)}/{attempted} saturation connects succeeded"
            )
        accepted_count = len(sockets)
        rejected_count = attempted - accepted_count
        if rejected_count == 0:
            raise SystemExit(
                "listener-recovery: FAIL: pressure was inconclusive; no connection attempt was rejected"
            )
        time.sleep(1.0)
        for held in sockets:
            try:
                held.setsockopt(
                    socket.SOL_SOCKET,
                    socket.SO_LINGER,
                    struct.pack("hh", 1, 0),
                )
            finally:
                held.close()
        sockets.clear()

        deadline = time.monotonic() + args.recovery_timeout
        recovered = False
        while time.monotonic() < deadline:
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.settimeout(args.connect_timeout)
            try:
                probe.connect((args.host, args.port))
                recovered = True
                break
            except OSError:
                time.sleep(0.25)
            finally:
                probe.close()
        if not recovered:
            raise SystemExit("listener-recovery: FAIL: listener did not recover")
    finally:
        for held in sockets:
            held.close()
        time.sleep(0.5)
        stop_reader.set()
        reader.join(timeout=1.0)
        serial_port.close()
        if captured:
            print(captured.decode("utf-8", errors="replace"))

    log_text = captured.decode("utf-8", errors="replace")
    if "closing the server socket" in log_text:
        print(log_text)
        raise SystemExit("listener-recovery: FAIL: firmware closed the server socket")
    print(
        f"listener-recovery: PASS ({accepted_count} held connections released; "
        f"{rejected_count} rejected; {attempted} attempted)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
