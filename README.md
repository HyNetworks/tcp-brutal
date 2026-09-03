# ![TCP Brutal](logo.png)

TCP Brutal is [Hysteria](https://hysteria.network/)'s congestion control algorithm ported to TCP, as a Linux kernel module. Information about Brutal itself can be found in the [Hysteria documentation](https://hysteria.network/docs/advanced/Full-Server-Config/#bandwidth-behavior-explained).

As an official subproject of Hysteria, TCP Brutal is actively maintained to be in sync with the Brutal implementation in Hysteria.

## For users

Installation script:

```bash
bash <(curl -fsSL https://tcp.hy2.sh/)
```

Manual compilation and loading:

```bash
# Make sure kernel headers are installed
# Ubuntu: apt install linux-headers-$(uname -r)
make && make load
```

### NixOS

If you are using NixOS with flakes, you can add the module directly to your `flake.nix`:

```nix
{
  inputs.tcp-brutal.url = "github:apernet/tcp-brutal";
  
  outputs = { nixpkgs, tcp-brutal, ... }: {
    nixosConfigurations.myHost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        # ... your configuration.nix ...
        tcp-brutal.nixosModules.default
        { boot.tcp-brutal.enable = true; }
      ];
    };
  };
}
```

> Kernel version 5.10 or later is required.

### Do I need to use a specific proxy protocol?

No. TCP Brutal works with any TCP-based protocol. But it requires both the client and server software to support it. If yours doesn't, go ask the developers to add support loudly!

If you are a developer, see the "For developers" section below.

### Speed test

The [example](example) directory contains a simple speed test server+client in Python. The client opens several connections that share one rate as a group. Usage:

```bash
# Server, listening on TCP port 1234 (requires the v2 module)
python server.py -p 1234

# Client, connect to example.com:1234, download at 50 Mbps in total
# over 4 connections (-n) for 10 seconds (-t)
python client.py -p 1234 example.com 50
```

### Do not set TCP Brutal as your system's default congestion control

Unlike standard congestion control algorithms, TCP Brutal only works properly when the application sets the target bandwidth through a special socket option, which requires explicit support.

If you set TCP Brutal as the system default, connections from unsupported applications will be limited to 1 Mbps. Applications that support TCP Brutal will enable it automatically when needed, so there's no reason to set it as the system default.

## For developers

This kernel module adds a new "brutal" TCP congestion control algorithm to the system, which programs can enable using TCP_CONGESTION sockopt.

```python
s.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, "brutal".encode())
```

To set the send rate, congestion window gain (we recommend a default value of 1.5x to 2x, which is expressed as 15/20 since the kernel doesn't support floating point) and optionally a group:

```c
struct brutal_params
{
    u64 rate;      // Send rate in bytes per second
    u32 cwnd_gain; // CWND gain in tenths (10=1.0)
    u64 group_id;  // 0 = rate applies to this connection only (v1 behavior)
} __packed;
```

```python
TCP_BRUTAL_PARAMS = 23301

rate = 2000000 # 2 MB/s
cwnd_gain = 15
group_id = 42
brutal_params_value = struct.pack("<QIQ", rate, cwnd_gain, group_id)
conn.setsockopt(socket.IPPROTO_TCP, TCP_BRUTAL_PARAMS, brutal_params_value)
```

The 12-byte v1 struct (without `group_id`) is still accepted. The same option can be read back with getsockopt; a group member reports the group's rate, cwnd_gain and group_id:

```python
rate, cwnd_gain, group_id = struct.unpack("<QIQ", conn.getsockopt(socket.IPPROTO_TCP, TCP_BRUTAL_PARAMS, 20))
```

To check that the module supports groups, read the module version with getsockopt on a connection that already uses brutal. Older modules (and plain TCP sockets) fail with `ENOPROTOOPT`:

```python
TCP_BRUTAL_VERSION = 23302

# u32: major << 16 | minor << 8 | patch
version = struct.unpack("<I", conn.getsockopt(socket.IPPROTO_TCP, TCP_BRUTAL_VERSION, 4))[0]
supports_groups = version >= 0x020000
```

### Groups

All connections that set the same non-zero `group_id` (from the same user and network namespace) share `rate` as their **total** send rate. Bandwidth is not divided statically: a connection that is the only one sending gets all of it, and connections that send less than their share leave the rest to the others. **Setting params on any member updates the whole group's rate.** A group exists as long as at least one member connection is open.

A typical proxy server puts all connections belonging to one client into one group, keyed by that client's identity, so the client's bandwidth setting is enforced across all of its connections.

### For proxy developers (important)

Brutal only works when it knows the bandwidth of the connection, and most TCP proxy protocols do not have a way for the client and server to exchange that information. We suggest using the "destination address" field that every proxy protocol has: a client that supports TCP Brutal requests a connection to a special address such as `_BrutalBwExchange`, and if the server accepts, both sides exchange their bandwidth over that connection.

**TCP Brutal v1 had no concept of groups: the rate applied to each connection individually, so it was only usable with protocols that multiplex all proxy connections into one TCP connection (mux). v2 removes this limitation. For protocols that open a TCP connection per proxy connection, put all connections of the same client into one group so that their combined rate stays within the client's bandwidth. Without a group, v2 behaves like v1.**

### Compatibility

TCP Brutal is only a congestion control algorithm; it does not change the TCP protocol, so either side can use it without the other. It controls sending, and proxy users mostly download, so running it on the server alone gives most of the benefit. A client with TCP Brutal would upload faster, but most users are on Windows, macOS or phones where installing a kernel module is impractical.
