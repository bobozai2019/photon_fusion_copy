# Core 与传输插件

Windows x64 SDK 提供 v1 C 表、纯 TCP 模块、Direct 兼容模块和宿主适配器。[改造方案](../../task/transport-providers/改造方案.md) · [C 接口](../../task/transport-providers/传输接口.md)

| 主张 | 依据 | 边界与实现影响 |
| --- | --- | --- |
| 传输驱动按消息接受前缀，Core 保留未提交的后缀 | C/C++ 夹具与限额会话夹具可以分别观察本地接受、待提交字节和关闭事件 | 游戏层的 RPC 和属性确认沿用业务序号与版本。Core 按对端维持发送顺序，只对相邻同通道条目分批。驱动队列与 Core 待提交队列各有字节预算。 |
| 保持 Direct 的原地址和端口调用路径 | 相同消费程序链接两份 Core，并交叉组房。所采业务载荷与通道一致 | `Netcode::Direct` 继续提供原直连接口。Direct 模块同时可以通过 C ABI 装载。Direct 的 UDP 通道仍允许掉包。 |
| Provider 随会话打开一次，备用房主连接由同一工厂打开 | 工厂拥有 DLL 的引用及配置，连接实例释放后才卸载模块 | 新接口默认选择 TCP。外部模块通过显式 ID 和路径选中。Windows 相对模块路径以调用宿主的原生模块目录为基准。Godot 的 `res://` 与 `user://` 先解析成磁盘路径。 |

另一种组批方式是在网络上给一组业务消息增加新封套。可在同一属性负载中计数业务消息、驱动批次、系统调用与排队年龄，区分合批接口的收益和新线上格式的收益。现有 v1 每个条目仍独立成帧，保留不同驱动的消息边界。

TCP 接受数字 IPv4 加端口或带方括号的数字 IPv6 加端口。可选配置是 UTF-8 `queue_bytes=65538` 一类的字节值。帧由一个通道字节、二字节网络序长度和业务负载组成。默认每 peer 队列 512 KiB，容许最大长度的 Control 帧。直连可选配置 `timeout_ms=3000`，Control 和 Game 的业务负载均须非空。

TCP 模块每次可把队首至多 16 个帧片段交给一次套接字写入，Windows 使用 `WSASend`，POSIX 使用 `sendmsg`。按套接字返回的字节数消耗原 FIFO，最后一个部分写入的帧保留偏移，统计仍在完整帧写出后结算。每 peer 每次轮询最多尝试写出 128 KiB。控制帧与游戏帧沿原字节流顺序交付。混合通道契约测试读取全部原始字节并检查关闭顺序。[Windows 写入接口](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend) · [POSIX 写入接口](https://www.man7.org/linux/man-pages/man3/sendmsg.3p.html) · [TCP 契约](../../tests/transport/tcp_contract.py)

新 API 由 Godot 的 `NetcodeGame.open_with_provider`、`NetcodeRoom.open_with_provider`、`NetcodeSpawner.open_with_provider`，Unity 的 `NetcodeGame.OpenWithProvider` 以及 Cocos 的 `GameComponent::create_with_provider` 提供。同一 provider 配置可用来接回原席位。接回凭证属于客户端，经过应用自己保存和传回。Cocos 宿主把 SDK 的 `bin/netcode_transport_tcp.dll` 部署到应用可执行文件旁，Godot 导出时拷贝 DLL，Unity 的 UPM 包带 Win64 插件元数据。
