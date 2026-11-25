# 自研 Netcode 与 Photon Fusion Core v3 架构对比

从房间、输入和对象状态一路查到 Godot、Unity、Cocos2d-x 接入及掉线恢复，见[玩家联机同步全流程与技术选型对比](fusion-session-comparison.md)。本文保留逐 word 实现、迁移前抓包及早期车场的分版本证据。

**最接近 Fusion 的部分是状态复制。** 两边都把引擎绑定、权威状态、差量选择、接收应用分开，并用最后变更记录与确认进度驱动重发。**影响架构最大的差异是权威和分发拓扑。** Racing 由各对象所属客户端提交状态，经 Photon Cloud 分发。自研项目由同一个房主模拟整个世界，再直接同步给各客人。这个差异决定了输入怎么走、谁发 ACK、掉线后从哪里恢复。

自研侧以本仓库的 C++17 `core/`、车场运行时和 Windows x64 的 Godot/Cocos 适配器为准。原版以本地 **Fusion Godot Racing 3.0.0 Shared Authority** 随包源文件与指纹已核对的 Linux 原生库为静态证据。涉及原版其他拓扑、AOI 或重入 API 时，引用 Fusion Core v3 的公开契约。原版原生核心和云房间内部的完整源码未取得。[两套架构图与调用地址](architecture.md#12-fusion-core-v3-与自研-core-的源码对照) · [原版构建指纹](../research/fusion/builds/racing-3.0.0.md)

## 逐 word 复制实现

当前 Core 源码采用定长 `Words`、发送扫描的 `Shadow` 和逐 word 版本。房主下行按该客人的对象 ACK 筛选。客机持写权时按该代次的样本 ACK 筛选。Fusion Core 的公开契约使用 word 变更 tick。本项目使用各路径的单调版本，并以有界内联槽保存字符串。流程、布局和协商见属性同步规格。[本项目状态布局和差量](specs/property-binding.md#状态布局和差量) · [Fusion Core 对象与 Shadow](https://doc.photonengine.com/fusion-core/v3/manual/objects#shadow-buffer)

一个对象的 `Vector2` 只改一个分量时，按字段格式计算未压缩游戏负载为 63 B，逐 word 回归直接断言为 54 B。[包长回归](../tests/core/replication_test.cpp)

后续字段级数据与包长记录采用迁移前的车场基线。

## 1. 模块、进程与权威

| 层级 | Fusion Core v3 / Racing | 当前项目 |
| --- | --- | --- |
| 游戏与适配层 | Godot 的场景和脚本声明复制字段。原生 `FusionSharedReplicator` 把 Godot 属性读写映射到网络对象。随包 C# 包装转发给 Godot 原生类 | `replicators.cfg` 是两种引擎的共同字段契约。Cocos 的 `YardScene` 和 Godot 的 `NetcodeYard` 驱动同一个 `Runner`。Cocos C++ 或 Godot GDScript 负责节点生灭、路径、坐标和渲染 |
| 核心入口 | `FusionCore::Client` 组合对象、场景、RPC、兴趣和所有权，对外提供收包与发包的帧边界 | `netcode_core` 是库目标，提供 `AuthorityState / ReplicaState`、`RoomHost / RoomGuest` 和 `Rpc` 等组件。`examples/car-yard/Match` 组装并调度这些组件，车辆规则也在样例层 |
| 传输分层 | `FusionCore::Notify` 负责通道、分片、交付通知。原版二进制中 `PhotonNotifyPlatform::Send` 调用 `PhotonMatchmaking::RealtimeClient::SendEvent` | `Transport` 提供 `poll/send/close`。`netcode_direct` 是单独的 Windows Winsock 后端。房间和游戏分别走 TCP 与 UDP，另有内存后端供离线测试 |
| 权威部署 | Racing 使用 Shared。对象权威可由不同玩家持有，Photon Cloud 接收并分发当前状态。产品另有 Client-Server 模式，可由客户端房主或专用进程做模拟权威，分发仍经 Photon 房间 | 只有房主进程能提交整个世界的权威状态。车辆的 `owner` 把某位客人的输入与 RPC 绑定到这辆车。Cocos 或 Godot 都可以作为房主 |
| 房主离开 | Shared 的 Master Client 角色可重新分配。对象是否保留、迁移或销毁还取决于其所有权策略 | 房主是唯一世界权威。房主退出或失联后，客人结束该房间。当前实现的恢复范围是客人重连仍在运行的房主 |

Racing 中每个客户端都可以是自己车辆的写入者。场景 `vehicle.tscn` 的 `owner_mode = 1` 与随包 `FusionEnums.cs` 的 `PlayerAttached = 1` 对应，车辆脚本使用 `has_authority()` 决定谁写。自研车场中，`Match` 用连接识别驾驶者，再在房主模拟中写值。把引擎从 Godot 换成 Cocos，传输协议与房主写权保持一致。[Fusion 拓扑](https://doc.photonengine.com/fusion-core/v3/getting-started/modes-and-topologies) · [Godot 对象权威](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/replication/ownership-modes) · [本项目构建边界](../core/CMakeLists.txt) · [车场调度](../examples/car-yard/match.cpp)

## 2. 同一个油门字段怎样走完整链路

以客人在本机把输入从 `0` 改为 `0.75` 为例。这里统一数值做对照。Racing 的 `net_input_x` 是左右输入，自研车场的同名标记字段记录油门，两者的业务含义由各自示例定义。

| 阶段 | Fusion Racing Shared | 自研车场 |
| --- | --- | --- |
| 产生值 | 车的权威客户端在 `vehicle.gd` 读本机输入并直接写脚本字段。场景以 `NodePath(".:net_input_x")` 声明复制 | 客人把当前控制量提交到 `Runner / Match`。UDP `Input` 带递增序号、`throttle` 和 `steer`。房主按实际连接找到驾驶者 |
| 决定新状态 | 原生 Godot 绑定按节点与属性名调用 getter，将返回值编码到对象的固定 word 槽 | 房主拒绝未就绪、旧序号或越界控制量。约 30 Hz 固定步推进车的位置与转向，把新状态和控制值交给 `AuthorityState::capture` |
| 发出 | 发送阶段对 `Words` 和 `Shadow` 做扫描，`WriteDirtyWords` 选出尚未确认的变更，通过 Notify 发往云端 | 约 20 Hz 在 `AuthorityState::packets` 中逐客人比较字段版本和该客人的对象 ACK，直接发送 `Snapshot` |
| 应用 | Photon 房间把状态分发给相关客户端。客户端经 `StatePacketReceived → ReadObjectData` 合并其对象状态，再由 Godot 绑定写到场景 | 客人经 `ReplicaState::apply` 校验并合并完整数据报，发 `StateAck`。适配器随后把权威值应用到场景 |
| 平滑 | Racing 的自定义字段关闭单属性插值。根刚体的 `AUTO / Forecast` 另有物理校正路径 | `net_input_x` 等业务字段直接应用。视图的变换从当前显示值向新目标插值约 100 ms。旋转选最短角路径 |

因此，对自研车场按键后收到本机回写的 `0.75`，已经经过客人上行和房主模拟再下行。对 Racing 的对象权威端，脚本赋值后就可读取自己写入的值。它向云端提交后，其他客户端才收到该值。[原版属性绑定指令](../research/fusion/findings/property-binding.md) · [原版字段复制](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/replication/syncing-properties) · [自研输入与模拟](../examples/car-yard/match.cpp) · [Godot 显示应用](../examples/godot-yard/replicator.gd)

## 3. 对象身份、布局与绑定缓存

| 项目 | Fusion Core v3 | 当前项目 |
| --- | --- | --- |
| 身份 | `ObjectId` 公开结构为 16 字节，由创建者、地图和计数组成。`TypeRef` 用类型哈希与 word 数说明分配规格 | `ObjectId` 为 `u32`，同一局单调分配。游戏消息另带 `u64 RoomId` 和 `u64 RoundId`。`SchemaId` 与字段 ID 是 `u16` |
| 当前状态 | `Object::Words` 是定长 32 位 word 区，属性由固定偏移解释。公开布局预留 18 个尾部 word。具体属性的压缩与序列化由绑定决定 | `Object::values` 是 `vector<variant<float,bool>>`。二维、三维变换被配置展开为标量字段。单 schema 最多 64 个字段。线上只支持有限 `f32` 与单字节 `flag` |
| 存储核验边界 | Racing 的 Windows 插件在测试刚体上返回 `15、16、17、18` 个配置 word，分别对应 AUTO 加 0、1、2、3 个额外标量。这组观测描述 `compute_word_count` 的返回值，Core 对象总布局另依公开契约 | 活对象记录显式携带 schema ID、版本、字段 ID/类型指纹与 64 位掩码。接收方校验布局后按位解码，可拒绝本地配置和远端不匹配的记录 |
| 引擎引用 | 已核实 Linux 原生普通属性缓存保存节点指针、实例 ID、属性名和路径。常规采集经缓存调用 getter，把值写入 `base + 4 × offset` | 核心值没有引擎指针。配置给出路径，适配器按对象 ID 找本地实例，再从实例解析根或子节点。绑定与显示数据驻留在适配层 |

按内存职责看，Fusion 用两份同尺寸 word 数组承载当前值和上次扫描值，再配逐 word 变更记录。我们的权威对象用最近一次采样同时承担当前状态与下次比较基准。若比较实际内存，占用还取决于对象记录、容器容量、连接数和引擎侧缓存，单看一个字段的线上字节数无法回答。[Fusion 类型和布局](https://doc.photonengine.com/fusion-core/v3/manual/architecture) · [Fusion 对象缓冲](https://doc.photonengine.com/fusion-core/v3/manual/objects) · [原版布局探针](../research/fusion/probes/README.md) · [自研类型](../core/state/replication.hpp)

## 4. Shadow、ACK 与丢包重发

| 机制 | Fusion Core v3 权威端上行 | 自研房主下行 |
| --- | --- | --- |
| 比较发生在何时 | `CheckForMutatedState` 在发送阶段逐 word 对照 `Words / Shadow`。有差异时覆盖 `Shadow` 并记变更 tick | `capture` 采样时比较新旧字段值。若内容变化，对象 `revision` 加一，只更新对应 `changes[i]`，并保存新值和 `sample_tick` |
| 筛选条件 | `WriteDirtyWords` 使用 `wordChangeTick > ackThreshold`。原生库中确认回调 `PacketDelivered` 与 Shadow 扫描独立 | `packets` 使用 `changes[i] > peer.ack[object]`。每位客人的 `Progress` 保存 `baseline / sent / ack` 三张对象版本表 |
| ACK 的含义 | 当前权威客户端到 Photon 传输链路的已确认进度。某位对端什么时候应用到节点是后续问题 | `StateAck` 里的对象 ID 与版本代表客人副本已成功处理该数据报。旧记录可再确认，房主只单调推进到已发送过的版本范围内 |
| 重发内容 | 从最新 `Words` 读取仍待确认的数据。原版静态核实了扫描和筛选路径，线上双方的时间轨迹还需双端验证 | 下一次发送重新选最新字段。若同一字段又变，复制最新值。可靠 RPC 走自己的队列。单次状态报的数据报解析成功后才提交 |

自研示例中 B 和 C 都已有车辆。位置在版本 11 变化，油门在 12 变化。B 只确认到 10，C 已确认到 11。房主发送版本 12 给 B 时带位置与油门，给 C 时只带油门。B 对 12 的 ACK 丢失时，房主仍按 B 的确认进度筛选。B 识别重复版本并再次 ACK，已应用的状态保持单调。即使位置后来变回旧值，它最新的变更版本依然会进入尚未确认的差量。

Fusion 的对应演算是对象 word 在发送 tick 101 变更，Shadow 当场更新。tick 102 该 word 无新变化，但云端确认仍停在 100，已经登记的 101 仍可进入下一包。两套算法都把**上一次比较看到的值**和**对面确认到的进度**分别保存，因而能在没有新变化时继续补发。[Fusion Shadow 契约](https://doc.photonengine.com/fusion-core/v3/manual/objects#shadow-buffer) · [原版 Shadow 指令与确认布局](../research/fusion/findings/property-binding.md) · [自研采集、打包与 ACK](../core/state/replication.cpp) · [复制回归](../tests/core/replication_test.cpp)

## 5. 传输、包格式与 RPC

| 维度 | Fusion Core v3 | 当前项目 |
| --- | --- | --- |
| 控制与连接 | Realtime 负责连接、区域和房间。Notify 在其上承担游戏层通道。原版库能核实 `Notify::Connection`、`PhotonNotifyPlatform` 与 `SendEvent` 的路径 | 房主同一端口开 TCP 和 UDP。TCP 长度帧承载房间消息、250 ms 心跳和 UDP cookie。默认超时 3 s。UDP 按 cookie 与端点匹配实际连接，游戏头检查房间与局次 |
| 状态与投递 | Notify 的 Game 承载非可靠状态，Streaming 承载可靠投递，Unreliable 承载不可靠 RPC。通道层维护分片和交付通知 | 快照按游戏负载上限拆成独立完整的数据报。`Snapshot / StateAck` 服务状态收敛。`Rpc / RpcAck` 管调用序列，两个 ACK 用途独立 |
| RPC 寻址 | Core `CreateUserRpc` 接收目标玩家、目标对象、事件哈希和数据。Godot 包装支持对象目标或注册的广播接收器 | 方法表固定方法 ID、方向、作用域、可靠度和参数长度。方向沿用 Unreal 风格 `Server / Client / NetMulticast / Remote`，房间方法使用对象 ID `0` |
| RPC 顺序和重试 | 原版符号中可靠和不可靠 RPC 分别排队。公开 Notify 契约定义可靠分片与重发，本文只把该层证实的语义与自研配置对照 | 按发送方、接收方、对象或房间分别建可靠流。窗口 64、实例可靠队列上限 256、约每 100 ms 重试。累计 ACK 推进连续序列，断线或换局清流。跨流及属性之间可交错 |

**自研协议的字节例子。** `Snapshot` 的共同游戏头、发送 tick 和条数共 22 字节。每个活对象记录的固定部分为 33 字节，再加掩码选出的属性值。一个对象、一个 `f32` 变化的示例为 **59 字节游戏负载**，加 20 字节自研 UDP 封装为 **79 字节**。同样一个对象的 `StateAck` 是 26 字节游戏负载，加封装为 46 字节。数字来自字段宽度，取值粒度为一个数据报，范围止于 UDP 应用负载。游戏负载上限为 1,176 字节。Fusion 在线总包长受编码、通道与底层传输影响，原版在这里没有同场抓包数据。[Fusion Notify](https://doc.photonengine.com/fusion-core/v3/reference/notify-api) · [Fusion Core Client API](https://doc.photonengine.com/fusion-core/v3/reference/client-api#rpcs) · [Fusion Godot RPC](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/rpcs/basics) · [自研包头](../core/session/game_wire.hpp) · [自研 RPC](../core/session/rpc.cpp) · [Windows 传输](../core/transport/direct_win.cpp)

开门展示了状态和事件的职责。客人从自己的车发 `TryOpen`。房主校验车辆归属和距离，成功时改写门的 `is_open`，也可广播 `RoomNotice`。`RpcAck` 只表示收到对应可靠序列，开没开门从业务回复或权威字段判断。Fusion Godot 同样把迟加入需要看到的结果放到复制属性中，一次性 RPC 随调用投递。[房间与对象 RPC 规格](specs/rpc.md) · [开门实现](../examples/car-yard/match.cpp) · [Fusion Godot RPC](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/rpcs/basics)

## 6. 创建、销毁、兴趣与重连

| 场景 | Fusion Core v3 / Racing | 当前项目 |
| --- | --- | --- |
| 首次出现对象 | `FusionSpawner` / Core 创建对象，远端依据类型和生成信息建实例。原版库已核对 `StatePacketReceived` 经 `ReadObjectData` 处理对象数据，并可进入就绪通知 | 房主给对象分配新的单调 ID 与模板。未确认对象的客人收到完整字段。客人首次收到完整有效记录后，适配器按模板生成节点 |
| 销毁 | 原版状态处理含 `DestroyObjectFromRemote` 路径。Cloud 为晚加入者维护当前生成集合和状态 | 房主留下带更新版本的销毁记录并对有关客人送达。客人删除节点并保留销毁版本，迟到的已删除对象旧包无法复活节点 |
| 可见范围 | Core 支持对象兴趣策略、AOI、子对象与对象依赖。进入兴趣范围时按当前完整状态初始化 | 已就绪的客人接收全场对象。字段绑定到本地子节点，子节点仍隶属于同一个网络对象。当前没有动态 AOI 或单独的子对象网络协议 |
| 新玩家晚加入 | 开放的 Photon 房间可让晚加入者从云端收到当前生成对象与状态 | 房间等待时可加入。开局进入 `Playing` 后，普通新加入返回 `Locked`，本局旧成员走 `Resume` |
| 原成员重连 | Realtime Core 公开 `PlayerTtlMs`、保持身份与 `JoinRoomOptions::Rejoin`。具体对象的寿命与主人变化仍依其策略。Racing 本地未取得同条件在线恢复记录 | 房主核验 `RoomId/RoundId/PlayerId/Key`、成员离线状态，原玩家与原车不重建。客人的新连接从空副本和零 ACK 开始恢复。恢复票据目前保存在同一客人进程内 |

自研恢复还有一道**开回原车的门槛**。`attach` 为当时存活对象登记最小需确认版本。较新的有效对象版本也能达标，基线对象期间被销毁时用其销毁记录推进。基线达成后房主发 `Ready`，客人同时看到自己的原车才恢复发送控制量。恢复期间新生成的对象继续走常规快照。场景基线是这批对象的最低版本门槛，整个世界无需暂停等一份统一帧。[Fusion 生成和迟加入](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/spawning) · [Fusion Interest Area](https://doc.photonengine.com/fusion-core/v3/manual/interest-area) · [Realtime 房间重入](https://doc.photonengine.com/realtime-core/v6/manual/rooms-and-players) · [自研恢复校验](../core/session/room.cpp) · [自研恢复规格](specs/room-rejoin.md)

## 7. 帧循环、预测与显示

| 维度 | Fusion Core v3 / Racing | 当前项目 |
| --- | --- | --- |
| 帧边界 | 公共契约要求集成按 `UpdateFrameBegin → 引擎读取/模拟/写入 → UpdateFrameEnd` 前进。原版库核实 Begin 服务 Realtime、End 排队状态与 RPC。Racing 普通字段 getter 的每帧调用次数未测定 | `Match::update` 轮询网络，房主按固定节奏补模拟步，再走独立发送时钟及 RPC 重试。`sample_tick` 是采样时点，`send_tick` 是发送步，`revision` 只在对象状态变化时推进 |
| 控制与预测 | Racing Shared 由车主本地模拟并发布状态。Core v3 另在 Authority 模式提供 `QueueInput / ExecuteInputs`、序列和预测纠偏接口。Godot Forecast 是刚体的速度外推与纠偏路径 | 客人周期发最新控制量，房主拒绝旧序号，超过八个模拟步无新输入后用零输入。当前客人等待权威状态回写，使用显示插值。核心保留最新已提交状态。可重放的逐 tick 状态历史尚未实现 |
| 显示状态 | 已收到的权威 word、绑定解码状态以及刚体 Forecast 或显示平滑处在不同阶段 | Cocos 的 `from/to/display` 和 Godot 的 `previous/target/displayed` 与客人已提交的 `ReplicaState` 分开。到达时间与插值结束时间可分别观测 |

在延迟下，这一区别直接影响体验。我们的客人要经过上行、房主采样和下行才取得新车辆状态，然后画面平滑追上。Shared 权威车的本地输入可直接推进自身逻辑，远端刚体还能根据最近速度外推。Fusion Authority 的输入预测与纠偏是另一条路径，属于产品能力对照。[Fusion 帧循环](https://doc.photonengine.com/fusion-core/v3/manual/frame-loop) · [Fusion Ownership 与输入预测](https://doc.photonengine.com/fusion-core/v3/manual/ownership) · [Godot Forecast](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/physics-replication) · [Cocos 插值](../engineplatform/cocos2dx/replicator.cpp)

## 8. 扩展成本与已验证边界

| 问题 | 当前项目的实际边界 | Fusion Core v3 可对照项 |
| --- | --- | --- |
| 增加客人和对象 | `AuthorityState::packets` 逐客人遍历对象表，然后对需要发送的对象检查字段版本。本机验收覆盖 4 人、100 活动对象。公开互联网和更大场景缺少同等实测 | 有兴趣筛选、对象依赖、不同发送配置。SDK 和云房间内部的资源上限以及这个负载的实际吞吐未在本地比较 |
| 客人暂停与恢复 | 预设 1,050 ms 同进程离线试验中回到玩家 2 的原车。离线期间短命对象 6 被删，对象 7 保留。无故障与注入故障两轮分别约 1,216 ms 和 1,183 ms 恢复，轮内状态摘要一致 | Windows 实测覆盖 AUTO 配置布局，Linux 实测覆盖客户端静态调用和字段缓冲。原版在线探针在区域查询收到 `ApplicationNotFound`，本地没有可对比的恢复时延或跨端回写数值 |
| 应用彻底退出 | 关窗口或杀进程后，新进程当前发普通 Join，进行中收到 `Locked`。同进程 `ResumeTicket` 与活着的房主是已验证的恢复前提 | 可配置非活跃玩家保留及同身份重入。Racing 的实际断线对象留存和跨进程行为仍需匹配房间参数的运行证据 |
| 双引擎接入 | Godot 房主对 Cocos 客人，以及 Cocos 房主对 Godot 客人都已在同机跑过。共用层一次修改能覆盖两个适配器 | 官方 Core 面向不同引擎集成。本地静态证据的对象是 Godot Racing 的这一份构建 |

自研四端故障轮记录 150 次丢弃、207 次重复、186 次乱序。冻结后四端的摘要一致，摘要覆盖对象 ID、模板、所属和全部编码字段。这验证指定负载下的恢复与最终一致性。覆盖范围还包括开门 RPC、属性同步、出生销毁和跨引擎双向联机。[Godot 验收报告](../task/godot-platform/验收/验收.md) · [Cocos 验收报告](../task/cross-engine-sync/验收/M6.md) · [原版探针与错误](../research/fusion/probes/README.md) · [本项目架构与专项复测摘要](architecture.md#9-验证矩阵与实测)

## 9. 哪些设计借鉴到什么深度

| 判定 | 具体部分 | 对当前工程的含义 |
| --- | --- | --- |
| 机制级借鉴 | 无引擎状态层、引擎属性绑定、最近比较值、逐字段变更版本、独立确认进度、持续发最新值 | 能用同一组丢包和重发推演解释两边。编码长度、比较时机和确认链路由各自实现决定 |
| 职责级借鉴 | 状态、可靠事件和尽力事件有各自收发语义。加入或重新看到对象时以完整状态初始化 | 借鉴的是各链路该保证什么。我们的线协议格式、RPC 滑窗、TCP/UDP 划分、恢复票据和 `Ready` 流程由本项目定义 |
| 独立设计 | 房主集中模拟、四人锁局、按连接校验驾驶与 RPC、拒绝普通中途加入、全场基线恢复 | 把玩家身份、客人连接、车辆对象寿命和局次拆开，契合当前车场的部署和续玩规则。RPC 路由还参考了 Unreal 的执行方向 |
| 产品能力差距 | Cloud 房间与分发、兴趣筛选、多地图与子对象、多种权威策略及 Authority 模式下的预测接口 | 这些会影响部署、对象身份、可见集合或模拟时钟。自研目前完成的范围以仓库验收和明确的运行契约为准 |

这份对照支持把当前 Core 定位为**应用了 Fusion 核心复制思路的独立跨引擎实现**。最能一一对应的是 Shadow、变更版本与 ACK 的职责。对照时最应保持清楚的是两个逻辑边界，即 Fusion 的权威端对云房间与自研房主对某个客人。[本项目引擎边界](decisions/engine-boundary.md) · [传输设计来源](decisions/game-transport.md) · [首版运行契约](decisions/first-edition-runtime.md)
