# 房间管理与游戏传输 · 首版路线

本记录保留 Direct 首版选型及之后的可插拔传输设计。新游戏使用纯 TCP provider，Windows Direct 仍服务车场等场景。各后端通过同一 Core 传递 Control 和 Game 消息。[现行接入入口](../../README.md#新游戏使用纯-tcp) · [可替换传输决定](transport-provider.md)

**研究顺序**：优先核对 Fusion，未覆盖的问题再核对 UE；证据仍不足时，采用访谈第五轮的选项 1，并标出项目决定。

## Fusion 的依据

- 官方 Notify API 文档列出三个逻辑通道。Game（1）以不可靠方式承载状态更新；Streaming（2）提供可靠投递及重传；Unreliable（3）承载不可靠 RPC。Notify 通过平台接口向下投递，维护确认和分片。[Fusion Core Notify API](https://doc.photonengine.com/fusion-core/v3/reference/notify-api)
- 官方 Realtime 配置提供 UDP 或 TCP 选择。由 Notify 的通道表可以确定消息的投递类别，具体部署的底层传输还取决于配置。[Fusion Core Realtime API](https://doc.photonengine.com/fusion-core/v3/reference/realtime-api)
- 指定 Linux 编辑器库的 `Notify::Connection::SendChannel` 位于 `0x4898c0`。预测输入另由 `InputWritePending → InputFlushToServer → RealtimeClient::SendEvent` 发出。单条输入的入缓冲上限和两类 Realtime 事件的接收分派均已有指令依据。双端实际投递轨迹仍待测。[参考构建指纹](../../research/fusion/builds/racing-3.0.0.md) · [原生输入与 Notify 路径](../../research/fusion/findings/native-runtime.md)

## 本项目选择

房主使用 TCP 管理房间，游戏数据经自研 UDP 层在房主与客人之间直连。按 Fusion Notify 的职责划分状态更新、可靠 RPC、尽力投递 RPC，继续遵守已确认的 UE 风格 RPC 路由和单向业务语义。属性状态允许用更新的值覆盖旧值；验收时要覆盖最后一次变化丢失后仍能收敛。可靠 RPC 的确认、重传、去重、排序与属性状态的确认进度分别验证。

Direct 的输入同样经游戏 UDP 层上行。TCP 与 UDP 会话关联、序号及报文格式由本项目设计。车辆控制量的超时、最新样本规则和回归结果见运行时契约。[Direct 运行时契约](first-edition-runtime.md)

## 可选后端 Steam P2P

**实现决定。** 用 `ISteamNetworkingSockets` 实现现有 `Transport`。`Runner` 根据 `--transport direct|steam` 创建直连或 Steam 后端。房主监听 Steam 虚拟端口 `0`，客人用房主 `SteamID64` 连接。连接进入 `Connected` 后才发 `Open`。新连接获得新的本地连接身份，进程内 Steam 运行时可由重连前后的连接共用。[Steam P2P API](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets#CreateListenSocketP2P) · [后端实现](../../core/transport/steam_p2p.cpp)

**协议边界。** 一字节标记把现有消息分到 `Control` 和 `Game`。前者用 Steam 可靠消息，后者用 Steam 不可靠消息。`StateAck`、`RpcAck` 和应用层重发沿用原协议。Steam 的 `Connected` 只表示传输就绪。开局锁员、恢复凭证、场景基线和 `Ready` 继续由本项目的房间和状态层管理。发送拒绝或结束房间后即关闭时，Steam 后端在下一次轮询尝试刷新可靠队列并使用 linger。关闭前通知的实际送达顺序列入联机验收。[状态和关闭语义](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets#CloseConnection) · [房间与恢复规格](../specs/room-rejoin.md)

**备选方案。** 可把应用层可靠 RPC 映射成 Steam 自身的可靠消息。这会改变不同对象 RPC 的排序与背压边界。首版保持 RPC 流和确认逻辑，后续可用跨对象并发、重复包和 ACK 丢失场景比较两种方案。Steam 构建的 `control_*` 和 `game_*` 统计应用消息负载，直连构建沿用既有统计。

### Windows x64 开发构建

`--steam-sdk` 指向解包后包含 `public/steam` 和 `redistributable_bin/win64` 的目录，编译需要相匹配的头文件、`steam_api64.lib` 和 `steam_api64.dll`。运行时库复制到本地构建输出。开发阶段用有该 AppID 访问权的 Steam 客户端启动，或按 Steamworks 说明设置本地 `steam_appid.txt`。[官方 SDK 下载](https://partner.steamgames.com/doc/sdk) · [Steamworks 初始化](https://partner.steamgames.com/doc/sdk/api#initialization_and_shutdown)

在框架仓库根目录构建带 Steam 组件的 Core。

```powershell
python -B tools/build.py core --steam-sdk 'D:/deps/steamworks/sdk'
```

在同级示例仓库根目录构建消费该 Core 的车场。Cocos 构建还需匹配的 Cocos SDK。[示例构建说明](../../../examples/README.md)

```powershell
python -B tools/build.py --godot --cocos --steam-sdk 'D:/deps/steamworks/sdk'
```

- 房主启动参数为 `--transport steam --host`。客人用 `--transport steam --join <房主 SteamID64>`。Godot 从编辑器二进制启动项目时，把这些参数放在 `--` 之后。
- 入局在收到 Steam 连接就绪后继续走原房间消息。客人断开后按 R，以新连接提交原局 `ResumeTicket`。Steam 建连结果与实际已恢复到同一玩家、同一车辆是两个观测点。
- 本地编译使用公开 `steamworks-sys 0.13.0` 附带的 SDK 1.64 Windows 开发文件，按 crates.io SHA-256 校验。Steam 账号、AppID 下的可达性及中继联机仍待验证。现有直连脚本继续使用 IPv4，Steam 双端验收应核对 RPC 顺序、状态摘要、生灭和原车恢复。[本地编译依赖来源](https://github.com/Noxime/steamworks-rs)

## 独立连接模块

第六轮要求后续可以更换为纯 TCP、Steam P2P 或自研 P2P。保留 `core` 与 `engineplatform` 两个顶层；建议在 `core/transport/` 内为连接接口和可插拔后端建立单独的构建目标。连接层负责监听/建立/关闭、对端连接句柄、收发及断开事件；房间成员、对象、权威和 RPC 方法属于上层。房间身份与后端地址独立，具体主机地址、端口或平台标识仅进入相应后端配置。

上层声明消息的投递需求。首个后端在 TCP 与自研 UDP 通道之间分流；未来后端需说明可靠性、顺序及积压行为。尤其是纯 TCP 在丢包时会等待流重传；可丢弃尚未发送的过期状态，已经进入字节流的消息仍受队头阻塞。计划用内存后端和真实直连后端运行同一组会话测试，检查上层有无依赖特定套接字、地址形式或平台 SDK。

首版验收用同一台 Windows 机器、两个进程及可配置端口。纯 TCP provider 后续已通过 Windows x64 实机验收，Steam P2P 为可选编译组件。Steam 的账号联机和中继可达性沿用上面的验证范围。[纯 TCP 交付记录](../../task/transport-providers/执行状态.md)

## TCP 发送前的快照背压

**主张与依据。** TCP provider 的 C ABI 已能按连接返回两条通道的待发字节与最老队龄。Core 在准备属性数据前读取这些指标并加上自己的待发队列。跳过该连接的快照时，后续仍依据最新对象修订和该连接的 ACK 生成差量。测试覆盖一个连接的软水位、另一连接仍能构包、控制消息仍可提交，以及预算跨步时先前未发的对象取得最新修订。

**实现影响。** 64 KiB 待发窗口、50 ms 队龄和每发送步 32 KiB 上限是可靠有序后端的调度默认值。预算按未压缩包的上限计入。发送游标避免低对象号持续变化时饿死后面的对象。C 档压力测试还会持续收到客机的写入状态，房主将同一连接、同一对象的待发写入确认合并，再用原批次格式发出。写入确认按最新代次和序号收敛，玩法的拾取、升级、命中结算仍由游戏自行确认。Core 使用原 provider C ABI，TCP 两通道共享既有 FIFO。Godot 将硬阻塞、软水位和跨步构包计数暴露给项目诊断。[发送契约](../specs/game-session.md#可靠快照的逐连接发送预算)

**边界与备选。** 单 TCP 流中的内核缓冲和重传仍决定已交付字节的到达顺序。备选是给状态和可靠业务使用独立传输流。可在等价带宽下增加丢包，再比较拾取确认 P95 和状态收敛时间，区分构包积压与 TCP 顺序等待。[属性同步验收](../../task/godot-platform/验收/属性同步性能.md#幸存者操作积压与逐连接快照预算)
