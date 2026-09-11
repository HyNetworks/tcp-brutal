# <img src="logo.png" width="400">

TCP Brutal 是 [Hysteria](https://hysteria.network/) 的 Brutal 拥塞控制算法在 TCP 上的实现，以 Linux 内核模块的形式提供。关于 Brutal 算法本身的详细说明，请参阅 [Hysteria 文档](https://hysteria.network/zh/docs/advanced/Full-Server-Config/#_6)。

作为 Hysteria 的官方子项目，TCP Brutal 会持续维护，并与 Hysteria 中的 Brutal 实现保持同步。

**English: [README.md](README.md)**

> **v2 新特性：** TCP Brutal 不再需要上层应用专门适配。只需为某个目标地址设置一次速率，任何程序、任何基于 TCP 的协议，所有连向该地址的连接都会自动使用 Brutal。不必哀求开发者支持，现在你就能用！

https://github.com/user-attachments/assets/ba5f938b-265a-49a5-8b60-ce7efac0c6e2

## 快速开始

### 安装

```bash
bash <(curl -fsSL https://tcp.hy2.sh/)
```

该脚本会通过 DKMS 安装内核模块，并将 `brutalctl` 工具安装到 `/usr/local/bin`。需要 Linux 5.10 或更高版本。

如果使用带 flakes 的 NixOS，可以在 `flake.nix` 中加入该模块，同时也会提供 `brutalctl`：

```nix
{
  inputs.tcp-brutal.url = "github:HyNetworks/tcp-brutal";

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

### 为目标地址启用 Brutal

在**发送数据的一端**执行下面的命令。例如，如果用户从你的服务器下载文件，就应该在服务器上运行：

```bash
# 所有发往 203.0.113.5 的连接总共使用 100 Mbps，
# 无论这些连接来自哪个程序
brutalctl add 203.0.113.5/32 100
```

这里设置的是发往该目标地址的**所有连接合计速率**，通常填写接收方网络实际下载带宽。

如果当前只有一条活跃连接，它可以使用全部带宽；如果有多条连接，则会动态平衡共享。不需要上层应用提供额外支持：普通 Web 服务器、代理工具、rsync、SSH 等连接到该地址时都会自动生效。

```bash
brutalctl list                    # 查看规则，以及每条规则当前有多少连接
brutalctl add 203.0.113.5/32 50   # 修改速率；已有连接会立即使用新速率
brutalctl del 203.0.113.5/32
brutalctl flush
```

**连接只会在建立时匹配规则，因此应该先添加规则，再建立需要使用 Brutal 的连接。无需重启正在运行的程序，但程序已经建立好的连接不会应用新规则。已有规则后，如果对同一前缀再次执行 `add`，会实时修改整个连接组的速率；执行 `del` 后，新连接将不再匹配该规则，但已有连接仍会继续使用原来的速率，直到连接关闭。**

规则在重启后不会保留。如果需要持久化，请将相应的 `add` 命令加入启动脚本。

### 检查是否正常工作

可以从服务器下载文件并观察速率，也可以使用 [example](example) 中的测速程序。客户端会同时建立多条连接，并让它们作为一个连接组共享同一速率。

```bash
# 服务端，在 TCP 1234 端口监听
python server.py -p 1234

# 客户端，连接 example.com:1234
# 总下载速率设为 50 Mbps
# 使用 4 条连接（-n），持续 10 秒（-t）
python client.py -p 1234 example.com 50
```

示例程序会直接与内核模块通信，因此即使没有配置规则也能工作。**如果已经为客户端地址配置了规则，则以规则中的速率为准。**

## 工作原理

**Brutal 会按照你设置的速率发送数据。** 它不会像 CUBIC 或 BBR 那样主动探测可用带宽，而是直接按照配置的速率进行数据包 pacing。当发生丢包时，它会发送更多数据，以尽量让实际成功送达的数据速率维持在目标值。

因此 Brutal 的前提是：你大致知道路径上可用的带宽。如果把速率设得过高，只会制造额外的丢包。

**只需要部署在一端。** Brutal 控制的是发送行为，并不会改变线上传输的 TCP 协议，因此连接另一端不需要安装或支持任何东西。对于代理服务来说，大部分流量通常都是客户端下载，因此一般只在服务器端启用就能获得主要收益。

**连接组（Groups）。** 同一组中的连接共享一个总速率。带宽并不是静态地平均分给每条连接：哪条连接当前需要发送数据，就可以使用相应带宽；如果某条连接没有用完自己的部分，其余连接可以直接使用剩余带宽。

连接组有两种创建方式：

* 通过上面介绍的目标地址规则自动创建；
* 由应用程序直接为 socket 设置 group id。

**规则（Rules）。** 一条规则会把某个目标地址前缀映射到一个连接组。要让规则真正生效，需要同时完成两件事，`brutalctl` 会自动处理：

1. 将规则写入模块维护的 `/proc/net/tcp_brutal/rules`；
2. 添加一条路由，让内核在新建到该地址前缀的连接时自动选择 Brutal 拥塞控制。

对应的效果类似于：

```bash
ip route replace <prefix> via <gateway> congctl lock brutal proto 233
```

其中下一跳网关会从当前路由表中自动复制。

`brutalctl` 创建的路由都会带有 `proto 233`，因此可以通过下面的命令查看：

```bash
ip route show proto 233
```

`brutalctl` 不会修改其他路由。

如果你自行管理路由，例如目标地址通过某个策略路由表访问，可以使用 `noroute`，让 `brutalctl` 只管理规则本身。

当多条规则同时匹配时，使用**最长前缀匹配**。

**规则只对新连接生效。** 连接会在建立时加入对应规则的连接组。添加规则不会影响已经存在的连接；对已有规则再次执行 `add` 修改速率时，连接组中的已有连接会立即使用新的速率；删除规则后，已有连接仍会共享旧速率直到关闭，新连接则不再匹配该规则。

**默认锁定。** 规则默认会配合路由中的 `lock` 使用。启用锁定后，应用程序无法自行修改这些连接使用的拥塞控制算法或 Brutal 参数。

如果某个本身支持 TCP Brutal 的应用尝试设置参数，会收到 `EPERM`，此时应用应该直接继续工作即可。

如果希望允许应用程序自行设置 Brutal 参数，可以添加规则时使用 `nolock`。

**不要把 Brutal 设置成系统默认拥塞控制算法。** 对于既没有匹配规则、应用程序也没有主动设置参数的连接，Brutal 默认只会以 1 Mbps 发送。

支持 TCP Brutal 的应用程序可以自行在 socket 上启用，而普通应用可以通过规则覆盖，因此没有必要将 Brutal 设置成系统全局默认值。

## 开发者指南

### 在 socket 上启用 Brutal

```python
s.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, "brutal".encode())
```

然后设置发送速率、拥塞窗口增益，以及可选的连接组 ID。

建议的拥塞窗口增益为 1.5x～2x。由于 Linux 内核不能直接使用浮点数，这里用十分之一为单位表示，因此 15 代表 1.5，20 代表 2.0：

```c
struct brutal_params
{
    u64 rate;       // 发送速率，单位 bytes/s
    u32 cwnd_gain;  // CWND 增益，以十分之一为单位（10 = 1.0）
    u64 group_id;   // 0 = 速率只作用于当前连接（v1 行为）
} __packed;
```

Python 示例：

```python
TCP_BRUTAL_PARAMS = 23301

rate = 2000000 # 2 MB/s
cwnd_gain = 15
group_id = 42

brutal_params_value = struct.pack("<QIQ", rate, cwnd_gain, group_id)

conn.setsockopt(
    socket.IPPROTO_TCP,
    TCP_BRUTAL_PARAMS,
    brutal_params_value
)
```

v1 使用的、不包含 `group_id` 的 12 字节结构体仍然兼容。

也可以通过 `getsockopt` 读取当前参数。对于连接组成员，返回的是整个组的 `rate`、`cwnd_gain` 和 `group_id`：

```python
rate, cwnd_gain, group_id = struct.unpack(
    "<QIQ",
    conn.getsockopt(
        socket.IPPROTO_TCP,
        TCP_BRUTAL_PARAMS,
        20
    )
)
```

如果需要检查当前加载的是哪个版本的模块，可以在一个已经使用 Brutal 的连接上读取版本号。v1 模块以及普通 TCP socket 都会返回 `ENOPROTOOPT`：

```python
TCP_BRUTAL_VERSION = 23302

# u32: major << 16 | minor << 8 | patch
version = struct.unpack(
    "<I",
    conn.getsockopt(
        socket.IPPROTO_TCP,
        TCP_BRUTAL_VERSION,
        4
    )
)[0]

supports_groups = version >= 0x020000
```

### 连接组

同一用户、同一 network namespace 中，只要多个连接设置了相同的非零 `group_id`，它们就会共同共享 `rate` 作为总带宽。

在任意一个组成员上修改参数，都会更新整个连接组。

只要组中还有至少一条连接处于打开状态，该组就会继续存在。

典型的代理服务器可以把同一客户端的所有连接放进同一个连接组，并使用该客户端的身份生成 group id。这样，无论客户端建立多少条连接，配置的带宽上限都会作用于这些连接的总和。

TCP Brutal v1 没有连接组，因此只能用于所有流量都复用在单条 TCP 连接上的协议。v2 加入连接组后，也可以很好地支持“一条流一条 TCP 连接”的协议。

### 规则与应用程序

如果连接匹配了一条锁定规则，那么设置 `TCP_BRUTAL_PARAMS` 会返回 `EPERM`。

与此同时，由于对应路由也被锁定，即使当前连接实际上已经在使用 Brutal，下面的操作同样会返回 `EPERM`：

```python
setsockopt(TCP_CONGESTION, "brutal")
```

应用程序应该同时处理这两种情况：

如果收到 `EPERM`，可以通过 `getsockopt(TCP_CONGESTION)` 检查当前拥塞控制算法。如果已经是 Brutal，就无需再做任何设置，直接发送数据即可。

[example/server.py](example/server.py) 中提供了具体示例。

工具程序也可以直接操作规则文件，而不必调用 `brutalctl`。

读取：

```text
/proc/net/tcp_brutal/rules
```

每条规则占一行，以 `key=value` 形式显示，同时包含实时统计信息：

```text
dst=203.0.113.5/32 rate=12500000 gain=20 lock=1 id=1 members=3 sent=1834021376
```

写入时，每次 write 接受一条命令，其中速率单位为 bytes/s：

```text
add <prefix>[/<len>] rate=<bytes/s> [gain=<tenths>] [nolock]
del <prefix>[/<len>]
flush
```

如果对已有前缀再次执行 `add`，会直接更新原规则。

需要注意的是，路由配置是独立的一步。`brutalctl` 除了管理这里的规则文件之外，还会自动负责添加对应路由。

### 在代理协议中交换带宽信息

Brutal 必须知道目标带宽，但大多数基于 TCP 的代理协议本身没有客户端与服务器交换带宽参数的机制。

我们建议复用几乎所有代理协议都会提供的“目标地址”字段：

支持 TCP Brutal 的客户端可以请求连接到一个特殊地址，例如：

```text
_BrutalBwExchange
```

如果服务器识别并接受这个特殊目标，双方就可以通过这条连接交换各自的带宽信息。

### 从源码编译

```bash
make && make load   # 需要安装内核头文件，例如：
                    # apt install linux-headers-$(uname -r)

make -C tools       # 编译 brutalctl
```
