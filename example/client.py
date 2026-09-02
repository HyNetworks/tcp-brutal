#!/usr/bin/env python3
"""TCP Brutal v2 speed test client.

Opens several connections to the server, all in one Brutal group identified
by a random ID, and reports the download rate per connection and in total.
"""

import argparse
import os
import socket
import struct
import threading
import time

HEADER = struct.Struct("!QQI")
RECV_SIZE = 262144


def reader(sock, counts, index):
    try:
        while True:
            data = sock.recv(RECV_SIZE)
            if not data:
                break
            counts[index] += len(data)
    except OSError:
        pass
    finally:
        sock.close()
        counts[index] = -counts[index] - 1  # negative marks the connection as closed


def main():
    parser = argparse.ArgumentParser(description="TCP Brutal v2 speed test client")
    parser.add_argument("host", help="server host")
    parser.add_argument(
        "rate_mbps", type=float, help="total rate in Mbps for all connections"
    )
    parser.add_argument("-p", "--port", type=int, default=65432, help="server port")
    parser.add_argument(
        "-t", "--time", type=int, default=10, help="test duration in seconds"
    )
    parser.add_argument(
        "-n", "--connections", type=int, default=4, help="number of connections"
    )
    args = parser.parse_args()

    group_id = int.from_bytes(os.urandom(8), "big") or 1  # 0 would mean "no group"
    header = HEADER.pack(group_id, int(args.rate_mbps * 1e6 / 8), args.time)

    socks = []
    for _ in range(args.connections):
        sock = socket.create_connection((args.host, args.port))
        sock.sendall(header)
        socks.append(sock)
    print(
        f"group {group_id:016x}: {args.connections} connections, "
        f"{args.rate_mbps:g} Mbps total, {args.time}s"
    )

    n = len(socks)
    counts = [0] * n
    for i, sock in enumerate(socks):
        threading.Thread(target=reader, args=(sock, counts, i), daemon=True).start()

    print("   t " + "".join(f"{f'conn{i + 1}':>10}" for i in range(n)) + "     total")
    prev = [0] * n
    sums = [0.0] * n
    ticks = 0
    start = last = time.monotonic()
    while True:
        time.sleep(1)
        now = time.monotonic()
        row = f"{now - start:4.0f} "
        total = 0.0
        for i in range(n):
            count = counts[i] if counts[i] >= 0 else -counts[i] - 1
            mbps = (count - prev[i]) * 8 / 1e6 / (now - last)
            prev[i] = count
            sums[i] += mbps
            total += mbps
            row += f"{mbps:10.2f}"
        print(row + f"{total:10.2f}", flush=True)
        last = now
        ticks += 1
        if all(c < 0 for c in counts) or now - start >= args.time + 5:
            break

    print(
        "average: "
        + "  ".join(f"conn{i + 1} {sums[i] / ticks:.2f}" for i in range(n))
        + f"  total {sum(sums) / ticks:.2f} Mbps"
    )


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
