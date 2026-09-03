# <img src="logo.png" width="400">

TCP Brutal 是 [Hysteria](https://hysteria.network/) 的拥塞控制算法 Brutal 移植到 TCP 的版本，作为一个 Linux 内核模块。关于 Brutal 本身，请参阅 [Hysteria 文档](https://hysteria.network/zh/docs/advanced/Full-Server-Config/#_6)。

作为 Hysteria 的官方子项目，TCP Brutal 会持续与 Hysteria 中的 Brutal 实现保持同步。

**English: [README.md](README.md)**

## 用户指南

安装脚本：

```bash
bash <(curl -fsSL https://tcp.hy2.sh/)
```

手动编译并加载：

```bash
# 请确保已安装内核头文件
# Ubuntu: apt install linux-headers-$(uname -r)
make && make load
```

### NixOS

如果使用带 flakes 的 NixOS，可以直接把模块加进 `flake.nix`：

```nix
{
  inputs.tcp-brutal.url = "github:apernet/tcp-brutal";
  
  outputs = { nixpkgs, tcp-brutal, ... }: {
    nixosConfigurations.myHost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        # ... 你的 configuration.nix ...
        tcp-brutal.nixosModules.default
        { boot.tcp-brutal.enable = true; }
      ];
    };
  };
}
```

> 需要 5.10 或更高版本的内核。

### 需要换特定的代理协议吗？

不需要。TCP Brutal 可以直接用于任何基于 TCP 的协议。但需要客户端和服务端软件层面的支持。如果你用的软件还不支持，去催开发者加上吧！

开发者请见下面的「开发者指南」。

### 测速

[example](example) 目录里有一个 Python 写的简单测速服务端和客户端。客户端会建立多个连接，作为一个组共享同一速率。用法：

```bash
# 服务端，监听 TCP 1234 端口（需要 v2 模块）
python server.py -p 1234

# 客户端，连接 example.com:1234，开 4 个连接（-n）持续 10 秒（-t），
# 总下载速度 50 Mbps
python client.py -p 1234 example.com 50
```

### 请勿将 TCP Brutal 设为系统默认拥塞控制

和其他正常拥塞控制算法不同，TCP Brutal 需要上层应用通过专门的 socket option 设置带宽等参数才能正常工作。这需要上层应用的特殊支持。

如果 TCP Brutal 被设为系统默认，不支持它的程序的 TCP 连接都会被限制在 1 Mbps。支持它的程序会在连接层面自行启用，无需设为系统默认。

## 开发者指南

内核模块会向系统添加一个名为 "brutal" 的 TCP 拥塞控制算法，程序可以通过 TCP_CONGESTION sockopt 启用。

```python
s.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, "brutal".encode())
```

设置发送速率、拥塞窗口增益（推荐 1.5 到 2 倍；内核不支持浮点数，所以写作 15/20）以及可选的连接组：

```c
struct brutal_params
{
    u64 rate;      // 发送速率，字节/秒
    u32 cwnd_gain; // 拥塞窗口增益，以 0.1 为单位（10 = 1.0）
    u64 group_id;  // 0 = 速率只作用于当前连接（v1 行为）
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

为保持向下兼容，不带 `group_id` 的 12 字节 v1 结构体仍然可用。每个连接当前的配置可用 getsockopt 读回来，读到的是组的 rate、cwnd_gain 和 group_id：

```python
rate, cwnd_gain, group_id = struct.unpack("<QIQ", conn.getsockopt(socket.IPPROTO_TCP, TCP_BRUTAL_PARAMS, 20))
```

要确认模块是否支持组，可以在已经启用 brutal 的连接上用 getsockopt 读取模块版本。旧版模块（以及普通 TCP 连接）会返回 `ENOPROTOOPT`：

```python
TCP_BRUTAL_VERSION = 23302

# u32: major << 16 | minor << 8 | patch
version = struct.unpack("<I", conn.getsockopt(socket.IPPROTO_TCP, TCP_BRUTAL_VERSION, 4))[0]
supports_groups = version >= 0x020000
```

### 连接组

设置了相同非零 `group_id` 的所有连接（同一用户、同一网络 namespace）共享 `rate` 作为**总发送速率**。带宽会动态平衡分配：只有一个连接在发送时，它可以用满全部带宽；用不完自己那份的连接，剩下的会让给其他连接。**在任意一个成员上设置参数，都会更新整个组的速率。** 只要还有一个成员连接没关闭，组就一直存在。

典型用法是代理服务端以客户端身份为 key，将同一个客户端的所有连接放入同一个组，这样该客户端的带宽设置在它的所有连接上一起生效。

### 代理开发者须知（重要）

Brutal 必须知道连接的带宽才能工作，而大多数 TCP 代理协议都没有让客户端和服务端交换这一信息的机制。我们建议利用所有代理协议都有的「目标地址」字段：支持 TCP Brutal 的客户端请求连接一个特殊地址（例如 `_BrutalBwExchange`），服务端接受后，双方通过这个连接交换各自的带宽。

**由于 TCP Brutal v1 没有组的概念，速率只作用于单个连接，因此只能用于把所有代理连接复用到一条 TCP 连接的协议（mux）。v2 则不再有这个限制：对于每个代理连接单独建立一条 TCP 连接的协议，把同一个客户端的所有连接放进一个组，它们的总速率就不会超过设置的带宽。不使用组时，v2 的行为和 v1 一致。**

### 兼容性

TCP Brutal 只是一个拥塞控制算法，不改变 TCP 协议本身，所以客户端和服务端可以只有一方使用。拥塞控制算法控制的是数据发送，而代理用户以下载为主，因此通常只在服务端使用就能获得加速效果。客户端使用 TCP Brutal 可以提高上传速度，但多数用户用的是 Windows、macOS 或手机，安装内核模块往往不太现实。
