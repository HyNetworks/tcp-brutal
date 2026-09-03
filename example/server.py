#!/usr/bin/env python3
"""TCP Brutal v2 speed test server.

Each connection starts with a 20-byte header from the client:
group ID (u64), rate in bytes/s (u64), duration in seconds (u32).
The server puts the connection into that Brutal group and sends random
data for the duration. All connections sharing a group ID share the rate.
"""

import argparse
import errno
import os
import socket
import struct
import sys
import threading
import time

TCP_CONGESTION = 13
TCP_BRUTAL_PARAMS = 23301
TCP_BRUTAL_VERSION = 23302

CWND_GAIN = 15
HEADER = struct.Struct("!QQI")
CHUNK_SIZE = 65536


def brutal_version(conn):
    conn.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"brutal")
    raw = conn.getsockopt(socket.IPPROTO_TCP, TCP_BRUTAL_VERSION, 4)
    return struct.unpack("<I", raw)[0]


def check_module():
    """Exit unless the loaded tcp-brutal module is v2 (supports groups)."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        with socket.create_connection(listener.getsockname()):
            conn, _ = listener.accept()
            with conn:
                try:
                    version = brutal_version(conn)
                except OSError as e:
                    if e.errno == errno.ENOENT:
                        sys.exit("error: tcp-brutal kernel module is not loaded")
                    if e.errno == errno.ENOPROTOOPT:
                        sys.exit(
                            "error: tcp-brutal kernel module is v1, v2 is required"
                        )
                    sys.exit(f"error: {e}")
    if version < 0x020000:
        sys.exit(f"error: tcp-brutal kernel module is too old (0x{version:x})")
    print(f"tcp-brutal {version >> 16}.{(version >> 8) & 0xFF}.{version & 0xFF}")


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def handle(conn, addr):
    peer = f"{addr[0]}:{addr[1]}"
    try:
        header = recv_exact(conn, HEADER.size)
        if header is None:
            return
        group_id, rate, duration = HEADER.unpack(header)
        try:
            conn.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"brutal")
        except PermissionError:
            # A "congctl lock" route pinned the algorithm; fine if it is brutal
            cc = conn.getsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, 16).rstrip(b"\0")
            if cc != b"brutal":
                raise
        try:
            conn.setsockopt(
                socket.IPPROTO_TCP,
                TCP_BRUTAL_PARAMS,
                struct.pack("<QIQ", rate, CWND_GAIN, group_id),
            )
            print(
                f"{peer}: group {group_id:016x}, {rate * 8 / 1e6:.1f} Mbps, {duration}s"
            )
        except PermissionError:
            # A locked destination rule governs this connection: just send
            print(f"{peer}: governed by a destination rule, {duration}s")

        end = time.monotonic() + duration
        while time.monotonic() < end:
            conn.sendall(os.urandom(CHUNK_SIZE))
    except OSError as e:
        print(f"{peer}: {e}")
    finally:
        conn.close()
        print(f"{peer}: done")


def main():
    parser = argparse.ArgumentParser(description="TCP Brutal v2 speed test server")
    parser.add_argument("-l", "--listen", default="", help="address to listen on")
    parser.add_argument(
        "-p", "--port", type=int, default=65432, help="port to listen on"
    )
    args = parser.parse_args()

    check_module()

    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.listen, args.port))
        listener.listen()
        print(f"listening on {args.listen or '0.0.0.0'}:{args.port}")
        while True:
            conn, addr = listener.accept()
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
