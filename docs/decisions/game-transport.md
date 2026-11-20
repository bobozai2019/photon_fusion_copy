# 房间管理与游戏传输 · 首版路线

**研究顺序**：优先核对 Fusion，未覆盖的问题再核对 UE；证据仍不足时，采用访谈第五轮的选项 1，并标出项目决定。

## Fusion 的依据

- 官方 Notify API 文档列出三个逻辑通道。Game（1）以不可靠方式承载状态更新；Streaming（2）提供可靠投递及重传；Unreliable（3）承载不可靠 RPC。Notify 通过平台接口向下投递，维护确认和分片。[Fusion Core Notify API](https://doc.photonengine.com/fusion-core/v3/reference/notify-api)
- 官方 Realtime 配置提供 UDP 或 TCP 选择。由 Notify 的通道表可以确定消息的投递类别，具体部署的底层传输还取决于配置。[Fusion Core Realtime API](https://doc.photonengine.com/fusion-core/v3/reference/realtime-api)
- 对指定 Linux 编辑器库运行 `llvm-nm -D -C`，能定位 `Notify::Connection::SendChannel`（ELF 地址 `0x4898c0`）、`ReceiveAcksOnly`（`0x489070`）、`PhotonNotifyPlatform::Send`（`0x455f10`）及 `ObjectRoot::QueueInput`（`0x452dc0`）。这些符号证明该构建包含相应机制。输入队列符号本身无法确定输入的线上通道、冗余次数和丢失处理。[参考构建指纹](../../research/fusion/builds/racing-3.0.0.md)

## 本项目选择

房主使用 TCP 管理房间，游戏数据经自研 UDP 层在房主与客人之间直连。按 Fusion Notify 的职责划分状态更新、可靠 RPC、尽力投递 RPC，继续遵守已确认的 UE 风格 RPC 路由和单向业务语义。属性状态允许用更新的值覆盖旧值；验收时要覆盖最后一次变化丢失后仍能收敛。可靠 RPC 的确认、重传、去重、排序与属性状态的确认进度分别验证。

输入同样经游戏 UDP 层上行。连续控制量丢失时如何补偿、一次性动作如何确保只执行一次，作为单独的输入契约：先追 Fusion 输入发送/消费链路，仍未明确的部分再查 UE，然后记录项目策略。TCP 与 UDP 会话关联、序号及报文格式由本项目设计。首版不要求 Photon 线协议互通。当前状态为方案选定，代码与运行时验证待执行。

## 独立连接模块

第六轮要求后续可以更换为纯 TCP、Steam P2P 或自研 P2P。保留 `core` 与 `engineplatform` 两个顶层；建议在 `core/transport/` 内为连接接口和可插拔后端建立单独的构建目标。连接层负责监听/建立/关闭、对端连接句柄、收发及断开事件；房间成员、对象、权威和 RPC 方法属于上层。房间身份与后端地址独立，具体主机地址、端口或平台标识仅进入相应后端配置。

上层声明消息的投递需求。首个后端在 TCP 与自研 UDP 通道之间分流；未来后端需说明可靠性、顺序及积压行为。尤其是纯 TCP 在丢包时会等待流重传；可丢弃尚未发送的过期状态，已经进入字节流的消息仍受队头阻塞。计划用内存后端和真实直连后端运行同一组会话测试，检查上层有无依赖特定套接字、地址形式或平台 SDK。

当前实机验收用同一台 Windows 机器、两个进程及可配置端口。回环地址用于本地实验，监听和目的地址支持选用局域网网卡及对端地址。证据分别标为“同机真实套接字已测”或“跨设备实测待具备环境”；两种结论独立。纯 TCP 与两种 P2P 后端在后续里程碑实现。
