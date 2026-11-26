# Fusion Godot Preview 555 客户端拆解与项目差距

本次核验于 2026-11-20 至 21 完成。主样本是 Racing、Starter 和独立 SDK 三份发行物中相同的 Preview 555 插件。可读脚本已逐文件核对，Godot 直接注册的方法已逐项定位，Linux 编辑器库中两个核心命名空间的全部具名函数已有可复查的指令索引。属性、输入、刚体、权威、兴趣和场景的主要结论另经原版客户端入房验证。后续 Preview 684 的单机结果单列。以下只讨论客户端。[构建指纹](../builds/racing-3.0.0.md) · [复现命令](../probes/README.md)

## 拆到了哪里

| 对象 | 覆盖与核对办法 |
| --- | --- |
| 随包可读脚本 | Racing 5 个 GDScript、Starter 25 个 GDScript、两包共用的 17 个 C# 文件，共 47 个。逐个记录入口、谁写状态、哪里发事件、哪里只更新本地表现。[逐文件清单](sample-scripts.md) |
| Godot 公共入口和编辑器回调 | Windows 原版 ClassDB 的 21 个类、295 个直接声明的方法、93 项属性描述（含分组项）、23 个信号和 18 组枚举。对照 Linux 注册代码，295 个方法均有名字引用和实现入口。其中 234 项传给类型化绑定适配器，60 项找到成员指针写入，1 项经虚表解析。[450 条类型、签名与描述](godot-types.tsv) · [逐方法地址](godot-bindings.tsv) · [校验程序](../probes/native_index.py) |
| C# 到原生 | 13 个包装类声明的 242 个方法名、87 个属性名和 22 个信号名全部与原生注册匹配。另 4 个文件提供枚举或公共转换。方法名、委托签名和调用结果分别核对，授权信号确有一处差异，见下文。[包装层逐项说明](sample-scripts.md) |
| Fusion Core | Linux x86-64 编辑器 ELF 的 221 个 `FusionCore::` 文本符号合并为 210 个函数起点，按符号尺寸解码 21,308 条指令。索引保留地址、大小、签名别名、可解析的两命名空间出站调用和间接跳转计数。另有 35 个关键调用位置独立按重定位复核。[Core 函数索引](core-functions.tsv) · [关键分支与地址](native-runtime.md) |
| Photon Realtime 客户端 | 同一 ELF 的 156 个 `PhotonMatchmaking::` 文本符号合并为 150 个起点，解码 22,638 条指令。包括连接、区域、大厅、房间列表、属性、玩家视图、房间操作结果、事件收发与流量统计。[Realtime 函数索引](realtime-functions.tsv) |
| 平台范围 | 三份发行物的 17 个同名原生库逐个匹配 SHA-256。函数地址取 Linux x86-64 编辑器库。Windows x64 跑 ClassDB、布局和编辑器，Web 原版扩展与原版模板跑多人房间。各架构的符号数与运行版本见构建记录。[指纹和平台范围](../builds/racing-3.0.0.md) |

从接口往下看，`Fusion` 管连接、房间、RPC 和场景。`FusionSpawner` 管预注册、生成、销毁及子对象。`FusionReplicator` 与 Shared/Server 两个子类分别管绑定、状态权威与输入权威。房间、玩家、兴趣区、RPC 结果和重置信息有独立对象。编辑器、Inspector、Debugger、配置资源和 Schema 导出也注册自己的类型。以 `FusionServerReplicator.has_input_authority` 为例，Linux 注册点 `0x1e7684` 传入虚成员编码 `0x81`，对应虚表偏移 `0x80`、实现 `0x1e7140`，再由 `0x1e716b` 转入 Core 的权威判断。只看 C# 转发会漏掉这层分派。[注册表](godot-bindings.tsv)

Core 的 210 个函数起点中，111 个属于 `Client`，管帧循环、对象分配、状态包、输入、权威和地图。对象根、子与共同基类各有 10、3、8 个，Notify 连接有 19 个，字符串堆有 11 个，其余覆盖读写缓冲、时间、平台桥与 RPC 等。Realtime 的 150 个起点中，70 个属于 `RealtimeClient`，42 个属于接收异步结果和事件的 `Impl`，28 个属于可写房间视图。Godot 侧可沿 `0x179f9a` 到 Realtime Connect，沿 `0x174407 / 0x17452f / 0x174d2b` 到 Service、区域查询和选区，沿 `0x177d73 / 0x1714c6 / 0x171c6b` 到加入或创建、退房和断开。这解释了为何区域查询失败时业务层连第一个房间信号都收不到。[Core 函数](core-functions.tsv) · [Realtime 函数](realtime-functions.tsv)

## Shared 房间里的属性究竟怎样同步

用户提供的 Photon Cloud App ID 在 `asia` 区通过原版 Web 客户端入房。A、B 是同一机器上相互隔离的 Chromium 会话，分别取得玩家号 1、2。本机 HTTP 进程只提供引擎和场景静态文件。复制、权威和成员变化来自两个原版客户端连接的 Photon 房间。同一 ID 的 Windows 原生客户端在本机的区域查询阶段返回过 `1040`。这两条运行路径的结果分别记录。官方创建路径为 Photon Dashboard → Create a New App → Fusion → Version 3，Godot 配置在 Project Settings → Fusion → Connection。[官方快速入门](https://doc.photonengine.com/fusion-godot/v3-shared-authority/getting-started/quick-start-guide) · [探针入口](../probes/cloud_web.py)

使用两个相同脚本的 Node3D。实验对象注册 `score`，对照对象省略该字段。入房稳定后，业务脚本读单独的观测值，不调用被测 getter。保持字段不变，按物理步驱动 Fusion，一轮采样如下。[测量窗口](../probes/cloud_room.gd) · [属性计数器](../probes/cloud_actor.gd)

| 端与物理频率 | 物理步 | 渲染帧 | 实验对象 getter | 对照 getter | 实验对象 setter |
| --- | ---: | ---: | ---: | ---: | ---: |
| A 权威端，60 Hz | 60 | 41 | 60 | 0 | 0 |
| A 权威端，30 Hz | 60 | 84 | 60 | 0 | 0 |
| B 代理端，60 Hz / 30 Hz | 各 60 | 41 / 85 | 各 0 | 各 0 | 各 0 |

两段 A 窗口实际耗时 991 和 2,000 毫秒。改变渲染帧数后，采集次数仍跟随物理步。该结果限定在当前进程模式、已入房、拥有权威、普通浮点字段且数值稳定的条件。显式离线计算五次布局也会调用 getter 五次，离线空跑 30 个物理步得到零次。一次 getter、一次字段有变化、一次底层发包是三个独立观测量。[离线对照与 Shadow 路径](property-binding.md)

| 实验 | 客户端观测 | 对实现的约束 |
| --- | --- | --- |
| 晚加入与变量长度 | B 在 A 写入后才加入，第一份有效状态读到 `0.75`、owner 1。之后依次读到 `−0.5` 和 `0.25`。含表情的 270 个 Unicode 码点、864 个 UTF-8 字节以及四元素浮点数组逐值相等。随后又读回空串和缩短的两元素数组。 | 字符串的固定字段宽度和独立堆可同时成立。数组容量和当前有效长度也要分别表示。[原生堆路径](native-runtime.md) |
| 同帧三次写值与浮点精度 | A 在本地物理帧 1551 的同一回调依次写 `0.125、0.375、0.875`。B 的 setter 全窗口只触发一次，收到 `0.8799999952`。另将 `−0.124、0.376、−0.876` 分三次复制，B 分别读回约 `−0.12、0.38、−0.88`。实测项目设置 `float_compression=0`。 | 此配置下只交付最后采到的状态，四个样本与 0.01 精度量化一致。比较自研原样写入的有限 f32 时应规定观测误差。该样本不足以确定所有边界值的舍入算法。[写入与代理 setter](../probes/cloud_room.gd) · [自研浮点编码](../../../core/transport/wire.cpp) |
| 同路径替换子节点 | A 从树上摘掉旧子节点但保留实例，新节点占用原路径。旧值设成 11，新值设成 22。B 继续收到 11，A 新节点的 getter 为零。释放旧实例后 B 才收到 22。离线显式重新算布局时，同样替换由 float 变成 Vector3，旧实例尚存也立即由 2 变成 4 words。 | 在线热路径复用有效实例。Linux 出站路径 `0x19dc78` 起按实例 ID 回查并对指针，失效后经 `0x19dc9d` 进入重建。仅测试布局会错判实际复制的重绑时机。[字段缓存](property-binding.md) |
| 可靠与尽力 RPC | 同一对象的可靠调用在 B 收到 `0,1,2,3`，发送者均为 1。跨两个对象混发时，一轮次序为 tracked 的可靠 10、control 的可靠 20、tracked 的尽力 0、tracked 的可靠 11、control 的可靠 21、tracked 的尽力 1。 | 可靠与尽力通道各自排队，此处只给出实际到达序列。单次顺序无法推出跨对象、跨通道的全序或丢包下的公平性。[原生 RPC 和 Notify](native-runtime.md) |
| 用户兴趣键与销毁 | 对象使用键 42 和离开兴趣即销毁。B 订阅前看不到，订阅后读到 3，取消后本地节点消失。A 在此期间改为 7，B 重订阅得到新实例及值 7。普通对象由 A 主动 despawn 也在 B 消失。 | 退出视野允许同一世界对象再次出现。全局销毁和客人不可见需要不同的状态处理。[房间探针](../probes/cloud_web.py) |
| Transaction 写权 | A 的 GDScript 接受一个 `requester_id` 并返回布尔值。两个独立新对象分别测试拒绝与批准。拒绝后 B 保持代理，批准后 A、B 均读到新 owner 2。批准对象的 B 曾在同一个本地帧先收到 false 再收到 true。原生路径经 `0x1eac70` 取已连接 Callable，应答上没有业务请求序号。 | 以两端 owner ID 及 `has_authority` 判断交接。随包 C# 的 `Action<int,bool>` 有两个参数且无返回值，与这个原生请求及成功的 GDScript 回调签名有差异。托管 Player 的实际回调结果尚无运行证据。[原生信号签名](godot-types.tsv) · [C# 声明](../../../../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/cs/Replication/FusionSharedReplicator.cs) |

## 权威、物理、场景和成员寿命

**预测与纠偏已经在游戏进程间跑通。** 采用同一 555 插件的 Host 模式，普通玩家 A 担任 Host，B 获得输入权威。B 按物理步提交 60 个带不同序号的输入。A 执行 60 次、60 个不同序号、重复 0 次。一轮 B 执行了 476 次，其中 416 次重复处理已有序号，触发 26 次预测重置，最终队列为零。另外两轮的“总回调、重复、重置”分别为 544 / 484 / 28 和 434 / 374 / 24。三轮 Host 均只消费这 60 条独特输入。输入序号、权威端的已消费位置和预测端重置后再处理待确认输入，可从原生队列一直对照到脚本回调。重演次数随时序变化，不能拿来推算网络包数。[Host 探针](../probes/cloud_web.py) · [输入队列、去重和重演循环](native-runtime.md)

**Shared 刚体另走 Forecast。** 使用与原车相同的 Blend 模式建立无重力刚体，沿 x 轴以 3 移动，墙放在 3。A、B 都报告接触，位置约为 2.635、2.636，但两端该次观测的速度分别为 0 和 3。A 调用复制器传送至 25，B 读到 25，代理最大单步位移约 22.365。再唤醒后 B 读到速度 0.5，最后观察到睡眠和零速度。这是原版代理参与物理的可见效果。接触历史一致性或所有碰撞的逐 tick 误差还需要同步记录整条轨迹。[刚体设置与实验](../probes/cloud_web.py) · [Blend 与 Force 的实际代码分支](native-runtime.md)

**场景加载和对象 Ready 有两道不同的门槛。** A 增量加载空地图，B 收到同一地图序号 5 的 `scene_ready`，场景列表和指定挂载点子数为 `[1,1]`。卸载后 B 收到 `scene_unloaded`，二者为 `[0,0]`。对象方面，Linux 分配路径在 `0x45b31b / 0x45b8a8` 清对象 `+0x44`，本地子对象、普通根、全局及地图对象的成功路径分别在 `0x45fffc / 0x4623bc / 0x4626be / 0x462874` 设置。远端状态处理在 `0x4656bc / 0x465a13` 设置该标志。`TryFireObjectReady` 在 `0x4644a8` 检查被依赖对象的该位，依赖齐备后在 `0x464523` 写根对象自己的单次通知位 `+0x220`。这区分了依赖可用和业务通知已发。实验中的空地图没有人为制造依赖包的到达次序。[Core 索引](core-functions.tsv) · [对象就绪分支](native-runtime.md)

**Master 变化时，对象寿命取决于模式。** A 正常退房后，B 收到 Master 从 1 变 2，房内人数变为 1。MasterClient 对象改由 B 持有且保留值 8，原属 A 的 PlayerAttached 对象消失。新会话 C 使用 A 原来的 Photon User ID，按普通 join-or-create 再次加入，得到玩家号 3 并读到值 8。C 直接关闭浏览器，B 收到 `player_left(3,false)`。这些结果限于本次默认 TTL、正常退房和普通新加入的组合。555 的 Godot 公共入口暴露玩家 TTL 和普通加入。底层 Realtime 另有 Reconnect，显式 Rejoin 及非活跃玩家保留有不同前置条件。[会话实验](../probes/cloud_web.py)

## 编辑器生成的布局和运行时占用

通过原版 `FusionEditorPlugin` 的导出动作生成 JSON，同时在 Windows 原版中对相同场景调用 `compute_word_count`。字段为一个 float、一个 String、一组容量 4 的浮点数组。配置资源脱离对象时有三个未解析的类型行，`get_total_word_count()` 返回 0。用 `get_property_data → insert_full_property` 复制每行后，运行时布局与原配置一致。三个配置行序列化为 18 条属性描述。[编辑器探针](../probes/editor_fixture.py)

| Node3D 根模式 | 导出 JSON 的字段和 word 偏移 | 导出 words | 运行时 words |
| --- | --- | ---: | ---: |
| None | float 0，String 1，数组 3 | 8 | 9 |
| AUTO | 位置 0，旋转 3，float 6，String 7，数组 9，隐藏的 `_teleportTime` 14 | 15 | 16 |

两例里导出字段区和运行时计数都相差 1。额外 word 的用途不能从这两个总数反推。原版还导出了 `IScore` 与 `transform3d` 两个接口。调用 C# 导出时，编辑器明确提示本地交付未包含 `FusionSchemaCompiler` CLI。另取有独立指纹的 Preview 684，在 Windows 离线单机房验证了建房、玩家 1、对象权威、生成、销毁及退房。其单机稳定 60 个物理步内 getter 为零且无 shadow，和 555 双人房的条件不同。[编辑器与版本复现](../probes/README.md)

## 初次拆解时的项目差距

下表保留采集原版证据时的自研基线。本轮客户端实现与本机验收按能力另行更新。[现行实现与结果](../../../task/fusion-client-parity/执行状态.md)

| 能力 | 当前项目已经有的实现 | 由两边代码与实验确认的差距 |
| --- | --- | --- |
| 状态表示与引擎绑定 | C++ 核心保存字段值和变化版本，按客人确认进度重发。Godot、Cocos2d-x、Unity 共用房间与复制核心。 | 自研 `Value` 只有 float、bool，模板以最多 64 个字段的掩码打包。Fusion 支持声明容量的数组、字符串堆、变换、逐 word 比较。Godot 实例缓存则在旧实例失效后重建。自研 Godot 显示路径目前逐次按 NodePath 找节点。[自研字段](../../../core/state/replication.hpp) · [Godot 适配](../../../../examples/yard/godot/yard/replicator.gd) |
| 驾驶输入与响应 | 房主管模拟和权威状态，保存最近控制量，超出八个模拟步归零。客端显示做平滑。 | 预测需要按序号保留输入、回传已消费位置、在客端还原状态并重演。显示插值和重复输入回调可用上面的 60 条输入实验直接区分。三端的预测设计已列入仓库，运行能力仍看现有代码。[车场模拟](../../../../examples/yard/shared/match.cpp) · [项目物理设计](../../../docs/decisions/physics-prediction.md) |
| 权威与对象寿命 | 自研车辆的归属用于驾驶/RPC 校验，世界状态由房主写。客人恢复后可重新控制原车。 | Fusion 将状态权威、输入权威、Master 身份拆开，还按对象策略交接或销毁。自研按客人恢复原车、Shared 更换 Master、Host 给 B 输入权威，是三个可区分的场景。[房间与恢复](../../../core/session/room.cpp) · [项目对照](../../../docs/fusion-core-comparison.md) |
| 可见性、地图和 Ready | 已就绪客户接收全场对象，重连时等待场景基线与原车，销毁记录防止旧快照复活对象。 | Fusion 提供按兴趣键裁剪、重入完整基线、地图增卸、子对象与逐对象依赖 Ready。自研现有基线条件针对接收者整局，不能表达同一局内每个对象不同的可见集合和依赖。[自研复制](../../../core/state/replication.cpp) |
| 显示与物理 | 客人保留已提交状态和单独的显示插值目标，Cocos 车场约 100 ms 过渡。 | Fusion 的刚体 Forecast 可读写速度并处理传送、接触和睡眠。普通对象的显示插值、Shared 刚体外推和 Host 输入回放分别解决不同的本地误差。[自研 Cocos 显示](../../../../examples/yard/cocos2dx/replicator.cpp) · [原版物理分支](native-runtime.md) |
| RPC、联机和工具 | 自研已有可靠/尽力 RPC、对象/房间路由、局内恢复及三端同引擎验收。 | Fusion 还把编辑器配置、类型推导、Schema 导出、预生成回调和按权威执行连接在一起。路由细节、重试和跨流顺序各有自己的契约。两边没有同负载同链路的带宽、延迟或容量对照，无法据此排名。[自研 RPC](../../../core/session/rpc.cpp) · [项目验收索引](../../../docs/roadmap.md) |

这里的“逐项完成”有明确分母，即 47 个可读脚本、295 个注册方法、两个原生命名空间的 360 个具名函数起点。Linux Core 的解码仍含 103 处间接调用和 2 处间接跳转。匿名实现、第三方底层传输、其他架构的每条分支没有逐一动态执行。强制丢包下的 Notify 调度、依赖晚到与显式 TTL 重入等也未取得同条件在线轨迹。索引、静态分支和实机结论可分别复核。这些边界均可仅用客户端描述。[运行命令](../probes/README.md)
