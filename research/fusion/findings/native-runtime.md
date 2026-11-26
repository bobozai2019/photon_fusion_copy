# Fusion Godot Preview 555 原生客户端拆解

本页保存 Preview 555 的关键原生指令与早期离线对照。完整注册表、Core 与 Realtime 两套具名函数索引，以及后来取得的原版 Web 房间轨迹，统一收在[客户端拆解与项目差距](client-audit.md)。

Racing、Starter 与独立 Preview 555 包里的 17 个同名原生库逐个 SHA-256 相等。下文的地址均为随包 **Linux x86-64 编辑器 ELF 虚拟地址**。本页离线实验使用 **Windows x64 编辑器 DLL、Godot 4.7.1、Racing 项目配置**。配套多人实验使用同一 SDK 的 Web 原版扩展和随包 Godot 4.6.1 模板。Racing 车辆走 Shared 模式，预测输入小节分析的是同一库中的 Client-Server 分支。[构建指纹与复核](../builds/racing-3.0.0.md) · [原版车辆场景](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/scenes/vehicle.tscn) · [官方拓扑契约](https://doc.photonengine.com/fusion-core/v3/getting-started/modes-and-topologies)

Linux 的动态符号中，按解名后以 `FusionCore::` 开头且类型为 `T/W` 计，有 **221 个条目、210 个不同起始地址**。构造和析构函数可共享地址。该口径排除了匿名或内联代码、部分模板、Godot 绑定和 `PhotonMatchmaking` 等其他命名空间。探针逐条校验了 **35 处选定的调用或尾跳**，核验方法是从指令目标跟到 PLT 槽，再用 ELF 重定位确定具名目标。两个数字分别度量可枚举的符号和选取的调用点，无法据此换算整体完成率。[静态探针](../probes/native_evidence.py) · [复现步骤](../probes/README.md)

## 随包脚本实际负责什么

| 可读层 | 已核实的入口和行为 |
| --- | --- |
| SDK 的 17 个 C# 文件 | Core 目录 5 个、Handles 目录 8 个、Replication 目录 4 个。`FusionWrapper` 保存底层 `GodotObject`，按方法名和 `Variant` 调用原生对象。对需要转换参数的事件，包装层把原生 Callable 和托管订阅者放在节点元数据中的同一个桥接对象里。通过另一个包装实例也能退订，最后一个订阅者移除时断开信号并移除元数据。`FusionSpawner` 暴露场景注册、生成、地图绑定和子对象入口，实际对象表由原生侧维护。[包装基类](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/cs/Core/FusionWrapper.cs) · [生成器](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/cs/Replication/FusionSpawner.cs) |
| Racing 的 5 个 GDScript | 连接界面提供入房信号，主场景预注册两种车辆场景，入房后按选择生成并指定相机目标。车主读取方向输入并发布三个配置字段，代理车的脚本从同步字段取输入，车身模型的朝向另有脚本平滑。根刚体同时有独立的原生 Forecast 配置。[生成入口](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/scripts/main.gd) · [车辆逻辑](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/scripts/vehicle.gd) |
| Starter 的 25 个 GDScript | 第三人称 2 个、平台跳跃 7 个、射击 7 个、共用 9 个。跟踪了两个包含即时反馈的业务调用链。平台跳跃的 `coin.gd` 向金币 owner 请求拾取，本地先隐藏。金币配置为 MasterClient，处理者检查 `net_active` 后启动冷却，并定向 RPC 通知获奖者。射击的 `health.gd` 先在非权威端播放受击反馈，权威端更新场景配置的 `net_health/net_hits`，本地用 `_local_hits` 避免回写同一命中时重复触发。两处业务都能提供即时反馈，单凭画面无法判断底层是否重演过输入。[Starter 原包](../../../../references/photon/distributions/photon-fusion-godot-starter_3.0.0.zip) |

## 1. 属性布局、getter 与字符串

**Windows 原版实测。** 新建根节点和 `FusionSharedReplicator`，保持离线，浮点压缩配置为 `0`。下表的单位是 `compute_word_count(root)` 返回的 32 位 word。每个根节点均先测 None，再测 AUTO，两次使用同一对象。

| 根节点 | None | AUTO |
| --- | ---: | ---: |
| Node | 1 | 2 |
| Node2D / Node3D | 1 / 1 | 5 / 8 |
| CharacterBody2D / CharacterBody3D | 1 / 1 | 7 / 11 |
| RigidBody2D / RigidBody3D | 3 / 3 | 9 / 15 |

在 `Node3D + None` 的 1-word 基础上，每次只配置一个字段。`float`、`bool` 各增加 1 word，`int`、`String` 各增加 2，`Vector3` 增加 3，`Quaternion` 增加 4。字符串从 5 个 ASCII 字符加到 256 个时，对象配置均为 3 words。`Array[float]` 的配置容量为 2、4、8 时，配置总量分别为 4、6、10 words，同一容量下把当前数组从空改为两个元素，结果相同。这个对照支持按声明容量预留槽位。网络发送字节数、数组其他元素类型及扩容后的兼容性要分别测量。[布局探针](../probes/native_layout.gd)

**getter 可由布局计算触发。** 脚本给同一个浮点 getter 计数。显式调用 5 次 `compute_word_count`，未配置该字段时读数为 0，加入该字段后读数为 5。停止显式计算，保留该字段并等待 30 个离线物理帧，读数为 0。由此可排除在这个离线场景中每个物理帧固定采集该字段的解释。原版 Web 稳定运行时，60 和 30 Hz 下 A 的 60 个物理步各读 60 次。相同脚本省略该字段、或在稳定代理端测量，结果均为零。[在线窗口与对照](client-audit.md) · [属性缓存与采集指令](property-binding.md) · [双端标记脚本](../probes/shared_float.gd)

**Linux 原生补足了字符串长度实验的解释。** `Object::AddString` 在 `0x45294b` 转入堆分配，`Object::FreeString` 在 `0x45296b` 转入 `FreeHandle`。`WriteObjectRoot` 先检查普通 words，再在 `0x462dd3 / 0x462ded` 检查并写字符串堆。`CheckMutatedStringHeap` 在 `0x45a8c8` 起逐字节比较两份缓冲，变化时更新对应的变更记录。接收端在 `0x46569d / 0x4656af` 分别处理对象数据与堆数据。一个可替代的解释是固定槽中内联短字符串，超长后截断。运行观测给出槽尺寸，堆的独立收发路径与官方句柄契约提供了额外证据；实际 Web 双端逐值读回了含表情的 864 字节 UTF-8 字符串、空串和两种有效数组长度。[往返观测](client-audit.md) 自研 `Value` 扩到字符串时，需要定义对象内引用、字符数据的寿命和重连时如何补齐两者。[Core 字符串堆](https://doc.photonengine.com/fusion-core/v3/manual/string-heap) · [自研状态类型](../../../core/state/replication.hpp)

## 2. 赛车刚体 Forecast 与 Client-Server 输入预测

### Shared 赛车的远端刚体怎样校正

在 Windows 原版中离线实例化 `vehicle.tscn`。读到 PlayerAttached、AUTO、Forecast，位置/角度传送阈值为 10 / 90°，外推重力关闭。场景未显式写入 `root_correction_mode`，实例和全新 `FusionSharedReplicator` 都返回 **2（Blend）**。原车读取的刚度、阻尼和最大外推时间为 50、10 和 **0.25 秒**。官方物理手册将 Force 列为默认值，因此复现本包赛车时应以该构建实际读值为准。[原车场景](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/scenes/vehicle.tscn) · [随包枚举](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/cs/Core/FusionEnums.cs) · [原版探针](../probes/native_layout.gd) · [官方物理手册](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/physics-replication)

Linux ELF 的类型信息把虚表槽 `0x6bfc30` 关联到 `FusionForecastHandler` 的处理入口 `0x15a300`。入口识别 RigidBody2D 和 RigidBody3D，并在正时间分支读取最大外推时间，在 `0x15ac55` 取两者较小值。3D 路径经 `0x15aad0` 调用校正函数。后者在 `0x1593c4` 读校正模式，0、1、2 分别跳入不同代码。**Blend** 分支读取 blend factor，并把计算结果与线速度、角速度交给 Godot 的属性写入入口。**Force** 分支读取弹簧刚度和阻尼，另有 `apply_central_force` 与 `apply_torque` 的 Godot 方法调用。该区别同时有类型信息、分支和实际消费者支持。观察本车离线参数无法确定在线碰撞后每一帧执行哪个校正分支。自研车场当前约 100 ms 的位置/旋转显示插值，也无法复现这些作用于刚体状态的路径。[静态复核命令](../probes/README.md) · [自研 Cocos 插值](../../../../examples/yard/cocos2dx/replicator.cpp)

### Client-Server 的输入发送、消费和重演

| 阶段 | 本构建中可复查的行为 |
| --- | --- |
| 进入队列 | `QueueInput` 先检查根对象 `+0x214 == 5`，再比对 `GetPredictingPlayer` 与 `LocalPlayerId`。随包枚举中 5 对应 `PlayerPredicted`。本机同时是 Master 的分支直接分派输入。其他合格客户端的分支分配并复制 payload，记录序号、delta time 和条目标记，可从 `0x452f48` 起跟踪。 |
| 选择待发输入 | 入队路径以 16 位值 `1` 初始化条目 `+0x18/+0x19` 两个字节。`InputWritePending` 在 `0x459125` 检查第二个字节是否小于等于 2，把序号、delta time 和数据写入缓冲后，在 `0x4591d3` 自增该字节。一条仍在队列里的输入，在这条路径最多被选入三次。提前移出队列或跳过发送分支时次数可以更少。 |
| 发送和接收 | `UpdateFrameEnd` 在房间、发送时机和模式条件满足时，经 `0x4641c8 / 0x4641d0` 写入并刷新待发输入。`InputFlushToServer` 调用 Realtime `SendEvent`，事件值为 `0x70`。Notify 平台发送使用 `0x65`。接收侧 `OnDataEvent` 的表在 `0x5a67c8`，二者分别进入 `InputPacketReceived` 和 `Notify::Connection::Receive`，对应尾跳为 `0x46683e / 0x466891`。 |
| 权威端接纳和消费 | `InputPacketReceived` 查找根对象，检查对象模式、输入记录对应玩家和已见序号。`0x45e53b` 起跳过旧或重复序号，新记录入队。`ExecuteInputs` 的 Master 分支在 `0x4533c6` 累计时间，达到队首输入的 delta 后才消费，并在 `0x453514` 将已处理序号写到对象尾部 `+0x3c`。 |
| 预测端纠偏 | 收到本机所预测对象的权威状态后，状态处理有一条按尾部 `+0x3c` 清理已处理输入的路径，见 `0x4663de` 附近。它能设置对象 `+0x2a4`。`ExecuteInputs` 在 `0x4535ad` 检查同一字节，进入重置回调分派，然后遍历保留输入再次分派，最后 `0x4539a2` 清标记。公开接口将两类回调称为 `OnPredictionReset` 和 `OnInput`。 |

“每次都发送最新控制值”和“保留未确认的输入并重演”在匀速行驶时都可能产生相似画面。入队复制、逐条计数、已消费序号和重演循环使这两种客户端结构可区分。三次是**单条输入进入待发缓冲的上限**。Web 双端为每条输入嵌入标记，一轮 Host 处理全部 60 条且没有重复，预测端处理 476 次并报告 26 次预测重置。单帧发包数量仍需传输层计数。[原版双端轨迹](client-audit.md) 自研 `Match` 目前保存驾驶者最近一次输入，超时归零。若要让客机立即响应转向并在回包时修正，应有可回放的输入记录、权威已消费位置以及将重置交给游戏模拟的接口。[随包预测接口](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/cs/Replication/FusionServerReplicator.cs) · [Core 输入与权威契约](https://doc.photonengine.com/fusion-core/v3/manual/ownership) · [自研驾驶逻辑](../../../../examples/yard/shared/match.cpp)

## 3. 可靠 RPC 与状态分开确认

`SendRpcInternal` 与 `SendRpcUnreliable` 都把本地玩家与递增序号写入 RPC，再调用 `Rpc::Write`。前者写 Client 的 `+0x348` 缓冲，后者写 `+0x370`。`PacketQueueRpc / PacketQueueUnreliableRpc` 在 `0x45a01f / 0x45a34f` 分别询问各自通道能否入队，成功分支把缓冲送入 `Notify::Connection::Queue`。Notify 构造函数把相应通道初始化为 2 和 3。接收时平台回调将 Game 通道交给状态解析，其余所示通道交给 RPC 解析。[Notify 通道契约](https://doc.photonengine.com/fusion-core/v3/reference/notify-api) · [Core RPC](https://doc.photonengine.com/fusion-core/v3/manual/rpcs)

丢失可靠分片时，`Ack` 在 `0x489ab4` 分派到 `ReliableLost`，它将分片接回通道队列并更新计数。`SendChannel` 随后读取同一队列的首项和计数。状态包送达走另一种处理，`PacketDelivered` 按原发送记录找对象，在 `0x4593ec` 写入确认进度，供后续脏字段筛选。由此能排除所有消息都按对象属性版本重发的解释。同对象可靠序号和一次跨对象混合到达顺序已有原版运行结果，拥塞下的公平性仍缺轨迹。[双端 RPC 实测](client-audit.md) 自研可以沿用状态与一次性事件各自的确认语义；项目自己的按对象可靠流、窗口 64 和重试间隔有独立的规格与代码。[状态确认依据](property-binding.md) · [自研 RPC](../../../core/session/rpc.cpp)

## 4. 对象就绪、所有权与可见性

| 问题 | 已定位的原生分支 | 对当前项目的含义和待验证点 |
| --- | --- | --- |
| 何时产生或复用对象 | 普通 `CreateObject` 分配对象并登记 ID。`CreateMapObject` 先在 `0x462737` 查同一根对象，命中后置输出标记并返回原指针，未命中才在 `0x46277e` 分配。状态接收在相应分支分别销毁根和子对象。 | 单局动态 ID 对车场足够。支持预置地图对象时需要稳定身份与绑定复用。真实场景卸载、重进、挂载替换后的引擎节点释放顺序仍待运行观测。 |
| 收到状态何时可交业务 | `TryFireObjectReady` 在 `0x464465` 检查根对象通知标记。随后读取依赖列表，按 16 字节 ID 逐个查找，并检查被依赖对象的状态字节 `+0x44`。查找失败或该字节为零时提前返回。满足其余条件后先在 `0x464523` 置已通知标记，再分派回调。`GetRequiredObjects` 从尾部取依赖数量，检查 word 容量后计算列表位置。 | 首个对象记录到达和业务就绪可以发生在不同时间。自研 `Ready` 面向某位客人的场景基线与原车。若支持相互引用的对象，还需对象级依赖和回调。已追到分配置零、本地创建与远端接收置位，以及独立的根对象已通知位 `+0x220`。[各写入地址](client-audit.md) |
| 请求写权何时生效 | `SetWantOwner / SetDontWantOwner` 在符合前置条件时修改根对象 `+0x215` 的意愿值。`RespondToOwnershipRequest` 检查 `IsOwner`、请求者和模式后，通过 `0x459e2c` 发送内部 RPC。`HasInputAuthority` 在 Shared 路径比较 owner 与本地玩家，在 Authority 模式转入 `IsPredictingPlayer`。 | 申请意愿、实际写权、可提交输入是三个观测点。自研车的所属玩家可驾驶，整个世界由房主写状态。已有客户端拒绝、交接和 Master 离房后的所有者记录；同一对象同时有三方争用时的通知时序仍待测。 |
| 兴趣键如何表示 | 根对象设置路径把 Global 编为 0，User 编为 `2k`，Area 编为 `2k+1`。`SetInterestKey` 在 `0x458b68` 将编码写入尾部 `+0x24`。`GetInterestKeyType` 读取同一位置，由零和最低位还原类别。 | 客户端的键可表达空间或逻辑订阅。自研每客人遍历全场对象。加入 AOI 时还要定义离开视野与世界销毁的区别，以及重新进入的完整基线。客户端键编码不能揭示云端空间索引或调度算法。 |

“创建每个对象后立即 Ready”和“请求权威时直接替换 owner”都可以解释单车无竞争的运行画面。依赖延迟与两人同时申请能区分这些解释。随包 C# `AuthorityRequested` 还暴露了一个需要单独对照的细节。其注释描述回调返回批准结果，事件实际声明为无返回值的 `Action<int, bool>`。这条托管签名无法表达返回的授权决定。原生 ClassDB 声明只有一个 `requester_id`，GDScript 单参数回调返回布尔值，双端已观察到拒绝和转权。托管 Player 的回调尚无运行观测。[注册签名](godot-types.tsv) · [双端应答](client-audit.md) [随包 C# 声明](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/cs/Replication/FusionSharedReplicator.cs) · [Godot 权威策略](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/replication/ownership-modes) · [Core 兴趣键](https://doc.photonengine.com/fusion-core/v3/manual/interest-area) · [自研对象与基线](../../../core/state/replication.cpp)

## 5. 网络时钟实际怎样校正

`StatePacketReceived` 在 `0x465fc8` 把时间样本交给 `ServerTimeReceived`。`NetworkTime()` 和 `NetworkTimeScale()` 分别返回 Client 的 `+0x68 / +0x70`，`UpdateFrameBegin` 在 `0x4670f0` 起使用比例乘引擎传入的 delta 推进网络时间。按本构建该函数的比较和只读常量，当前网络时间为零时直接采用样本。后续误差按“收到的时间减本地网络时间”计算。

| 误差绝对值 | 该函数的写入 |
| --- | --- |
| `≤ 0.10 s` | 速率设为 1 |
| `> 0.10 s` 且 `≤ 0.25 s` | 本地落后时设为约 1.016667，超前时约 0.983333 |
| `> 0.25 s` | 直接采用样本时间，速率设为 1 |

支持数值结论的指令在 `0x4594a0` 至 `0x4594ef`，阈值和速率来自 `.rodata`。渲染目标变化时平滑追赶也能产生平滑画面，但这里有网络时钟自身的读写者与消费者。自研当前的固定模拟步、网络发送步和约 100 ms 的视图插值对应不同用途。原版服务器采样间隔、暂停/恢复及真实抖动下的时间轨迹还需另外观察。[Core 网络时间](https://doc.photonengine.com/fusion-core/v3/manual/time) · [自研渲染插值](../../../../examples/yard/godot/yard/replicator.gd)

## 6. 客户端运行证据与适用范围

| 范围 | 已有证据 | 下一条能区分实现的观测 |
| --- | --- | --- |
| 通用组件与工具 | 295 个直接注册方法均定位到 Linux 实现。555 编辑器导出的两份 Schema 已核对字段类型和偏移。684 另跑通离线单机建房、生成及退房。 | 编辑器 C# 导出依赖外部编译器。本地 555 仅验证到原版报告缺少 CLI 的阶段。[编辑器观测](client-audit.md) |
| 属性与 Forecast | 原版双端已测稳定采集计数、同路径换节点、长字符串与数组、刚体接触、传送、唤醒与睡眠。 | 改变发送率独立观察读值与发包，或逐 tick 对齐特定碰撞的历史，可更精确描述时序。 |
| 对象与权威 | 已定位依赖位与单次通知位。运行时测到 AOI 重入新实例、Master 交接、附属对象销毁、Transaction 拒绝及同意。 | 人为控制对象依赖晚到、多人同时争用时的最终次序仍缺同条件运行轨迹。 |
| 输入、RPC 与连接 | Host 实测 60 个不同输入各消费一次，预测端重置后重复回放。Shared 已记录跨对象混发 RPC、地图进出、同身份普通新加入及浏览器直接关闭。 | 强制丢包下的 Notify 调度、显式 TTL 重入和跨地区恢复需要各自的实验条件。 |

**平台观测。** 早期 Windows 原配置的尝试记录过 `32767 ApplicationNotFound`、`1043`、`1040`。换用经确认的测试 ID 后本机原生客户端在区域查询也出现 `1040`。同一 ID、同一 Preview 555 的 Web 原版经 WSS 在 Asia 成功建房与加入，取得了前述房间内结果。各次 Windows 错误没有统一根因证据。[客户端双端实测](client-audit.md) · [原版复现命令](../probes/README.md) · [自研验收矩阵](../../../docs/architecture.md)

**2026-11-23 Shared Authority 快速入门复测。** 在独立 Godot 工程里配置原版 Preview 555、同一 App ID、App Version `1.0` 和 `asia`。以 `Connect` 按钮的信号触发 `Fusion.connect_to_photon()`，仅在 `connected_to_photon` 后调用 `join_or_create_room()`。玩家场景包含 `FusionSharedReplicator`，根复制为 `Auto`，Snap Distance 为 `50`，并配置 `player_name` 和 `show_message` RPC。[Photon Quick Start](https://doc.photonengine.com/fusion-godot/v3-shared-authority/getting-started/quick-start-guide)

| 运行时 | 原版运行时观测 |
| --- | --- |
| Godot 4.7.1 Windows x64 | A 读到 App ID，收到 5 个区域并选择 `asia`，随后报告 `1040` 和断开原因 `5`。`connected_to_photon` 未触发。B 只记录了引擎启动。 |
| Godot 4.6.1 Photon Web 模板 | A、B 都从项目设置读取 App ID。在同一 `asia` 房间分配到玩家 1、2。双端收到对方生成的对象及 `player_name`，观察到远端移动超过 35 像素，各收到了对方发送的 RPC。 |
| Godot 4.6.2 Windows 导出模板 | 两个无界面进程打印引擎版本，在观测期内未到达场景启动标记。这组运行未触发 Photon 连接。 |

最早可确认的连接分歧发生在 Windows A 选区后、完成 Photon 连接前。Web 双端验证了 App ID 可用和指南场景的房间内行为。要进一步区分本机链路与 Windows 构建问题，需要让同一 Windows 构建在另一条网络链路上复跑，或获取选区后的原生传输日志。[Photon 断开原因表](https://doc.photonengine.com/realtime-core/v6/reference/enums)

### Godot 4.6.1 Windows 复测与同步试验场

经官方 SHA-256 校验的 Godot `4.6.1.stable.official.14d19694e` Windows x64 编辑器运行同一 Quick Start 和 Preview 555。两个 Windows 玩家都进入了 `asia` 房间。直接在 `room_joined` 回调调用 `spawner.spawn()` 时，B 端在 `network_time=0` 出现访问冲突 `0xC0000005`。延后 600 ms 的尝试仍在 `network_time=0` 遇到同类退出。同时间的 4.7.1 对照仍观测到 `1040`。随后 4.6.1 自身也多次复现 `1040`，版本选择不足以解释连接结果。

同步试验场复用 4.6.1 Windows 运行时。Fusion 等待仿真时钟推进后生成对象，入房前 `1040` 自动复试一次。通过 `eu` Cloud 房间的 128 对象双端验收中，两端各复试一次后成功入房。主端核对客端角色的八个属性，客端核对其余 127 个对象的八个属性，AOI 可见数为 128 → 76 → 128。Netcode 在同一场景经 TCP 直连以 512 对象验收，双端属性核对通过，客端 AOI 可见数为 512 → 268 → 512。两套后端发出的浮点与向量输入统一为 0.01 步长。[项目与复跑入口](../../../../examples/sync-lab/README.md)

Fusion 512 对象档在按客端已收到的对象数分批生成后，已观测到双方 `512/512` 可见、双向角色属性更新和房主端八字段核对通过。客端完整字段校验和 AOI 往返仍待同档确认。另一轮同条件被 Photon `1040` 阻断，结果保留为链路稳定性观测。[项目与复跑入口](../../../../examples/sync-lab/README.md)

**512 对象可见后的持续更新。** Godot 4.6.1 Windows 双端、原版 Preview 555、`eu` 区域。原负载每 100 ms 集中更新 A 持有的 510 个小球。复现时 A 本地角色继续推进，B 端收到的 A 角色版本停在第 635 轮，B 的远端状态年龄 P95 上升到 40.5 秒。B 自己的角色仍由本地更新。`512/512` 统计实例可见数。RTT 取连接往返时间，持续属性更新另用状态年龄和版本推进测量。

共同场景改为先同步玩家 5 秒，再由房主每秒轮转写入 120 个负载对象。Fusion 在 `asia` 的 512 档有一次通过八字段、`512 → 268 → 512` 和持续更新校验，末段 12 个采样的 P95 为 4.48–5.43 秒。另一轮 A 角色数据仍推进，批量状态年龄 P95 上升到 76 秒。当前证据可定位更新停止到 A→B 的属性到达阶段，Fusion 内部发送与 Cloud 分发的贡献仍待区分。128 对象档双端检查通过，B 端末段样本中负载年龄 P95 为 1.41–2.21 秒，因此试验场默认 128，保留高对象档复现。[试验场启动说明](../../../../examples/sync-lab/README.md)

**双端 SDK 字节率复测。** Godot 4.6.1 Windows、原版 Preview 555、`asia`、512 对象、客端订阅全部。试验场 `-Trace` 同时记录应用版本和 `fusion:stats` 中按对象报告的发送、接收字节率。默认轮转负载在 B 端 29 个连续采样窗都有小球版本推进，每窗约 1.1–1.2 秒，推进 104–184 个对象，状态年龄 P95 样本中位数 4.56 秒。原始全量每 100 ms 写入的对照里，B 的应用版本连续 23.55 秒停滞，P95 最后升至 27.34 秒。同段 B 端 17 个 SDK 样本中，A 所有者对象的接收字节率均为 `0 B/s`，A 同段 15 个 SDK 样本有 14 个报告非零发送率，中位数为 `35.3 KiB/s`。这些计数定位到 A 的 SDK 发送统计与 B 的 SDK 接收统计之间。实际出网、Cloud 转发、B 的底层接收和队列深度仍需独立观测。[试验场采集说明](../../../../examples/sync-lab/README.md#同步日志)
