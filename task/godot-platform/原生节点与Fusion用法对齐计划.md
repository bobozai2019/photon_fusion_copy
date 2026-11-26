# Godot Netcode 原生节点与 Fusion 用法对齐计划

2026-11-24 · T0–T6 已通过。Windows x64 SDK 跨版本验收与两轮用法复审见 [v5 验收](验收/T5.md#原生协议-v5)。Fusion 555 对照采用 Cloud Asia。

开发者在 Godot 的“添加节点”窗口搜索 `Netcode`，添加原生同步器或生成器，直接挂自己的 GDScript，再从 Inspector 选择要同步的字段。房主以 **TCP 监听 IP 和端口** 建房，客端以 **房主 IP 和端口** 加入。进入房间后，生成、写权、兴趣订阅和业务回调的用法参照 Godot 4.6.1 上的 Fusion Godot 3 Preview 555。

## 交付范围

| 阶段 | 交付给游戏开发者的能力 | 验收场景 |
| --- | --- | --- |
| 首个里程碑 | 原生节点可挂脚本，IP 直连，客端通过 Spawner 生成对象并设置初值 | 一个小球、一个字段、两个进程 |
| Shared 主交付 | Inspector 配置资源、多个模板共享布局、房间属性、Shared 写权、对象 RPC、用户兴趣键、完整 sync-lab 和可安装 SDK | 128 对象双端功能验收、512 对象持续负载、已有示例迁移回归 |
| 后续节点 | 空间兴趣区域和 ServerReplicator 的输入权威、预测与重演 | 移动观察者跨网格、Host 与预测客户端递交带序号输入，两项分别验收 |
| 范围外 | Photon Cloud 或协议互通、房间名匹配、原版资源零改动替换、整套 555 SDK 的逐项复刻 | 项目保留 Netcode 和 Fusion 各自的连接入口，支持退出房间后切换后端 |

## 原生类型与挂载方式

| 类型及 Godot 基类 | 放置方式与职责 |
| --- | --- |
| `NetcodeClientProcessor : Node` | 插件管理会话推进。每个活动会话只有一个推进者，多个 Spawner 订阅同一会话。 |
| `NetcodeEditorPlugin : EditorPlugin` | 在编辑器接入字段选择器、容量与插值设置、撤销及重做。 |
| `NetcodeReplicator : Node`（抽象） | 声明复制根、复制模式、配置资源、兴趣和通用信号。 |
| `NetcodeSharedReplicator : NetcodeReplicator` | 挂在游戏对象下，负责 Shared 权威和字段同步。可直接给它挂 `extends NetcodeSharedReplicator` 的脚本。 |
| `NetcodeSpawner : Node` | 接受 `PackedScene` 模板，在指定父节点下生成和销毁实例。 |
| `NetcodeInterestArea : Node` | 根据目标位置更新空间网格订阅。sync-lab 按组切换使用用户键。 |
| `NetcodeServerReplicator : NetcodeReplicator` | 管理输入权威、输入队列、预测执行和重演回调。 |

另由扩展注册 `Netcode` 引擎单例、`NetcodeReplicationConfig : Resource` 以及房间选项、房间视图和 RPC 结果对象。新场景将网络类型直接写入 `.tscn` 的 `type`。可保留现有编辑器控件和纯工具脚本；初始化、采样、回写和释放由原生插件负责，网络节点的脚本槽留给游戏。

```text
Pool
└─ Spawner (NetcodeSpawner)             spawn_path = ".."

Orb (Node2D)                            挂现有 orb.gd，直接读写玩法字段
└─ Sync (NetcodeSharedReplicator)       root_path = ".."，引用共享 .tres
```

## 脚本与数据契约草案

以下接口由原生 Godot SDK 提供。连接和生成时序分别见接入规格与 555 对照。[接入规格](../../docs/specs/godot-scene-sync.md) · [T0 对照](验收/T0.md)

| 操作 | 接口及可观察行为 |
| --- | --- |
| 建房与加入 | `Netcode.create_room(bind_ip, port, options)`、`Netcode.join_room(host_ip, port)`，IP 为字符串，端口为整数，复用现有 TCP provider。本机示例用 `127.0.0.1:43231`。立即返回 Godot `Error` 表示请求是否受理；连接建立或异步失败分别走 `room_joined()`、`connection_failed(reason)`。离房走 `leave_room()`、`room_left()`。测试非法端口、端口占用、连不上房主和退出后重开。 |
| 会话和房间属性 | `Netcode.get_room()` 提供玩家数、`get_custom_properties()`、`set_property(key, value)`、`set_properties(dict)`。房主初始化、客端更新、两端读到同一提交结果，晚加入者取得最新属性。sync-lab 用 `lab-size`、`lab-area`、`lab-progress`、`lab-ready`。入房、仿真时钟可用、游戏开始分别可判断。 |
| Inspector 与布局 | 首版对齐新建 Shared 节点的 `root_path=".."`、`root_replication_mode=NONE`、`owner_mode=Transaction` 和生成器的 `spawn_path=".."`。`.tres` 保存有序 NodePath、插值选项、适用的容量。三个小球模板共享布局，`trail`、`tags` 的容量均为 4。模板身份与字段布局分别标识，在两端按相同目录顺序解析。 |
| 生成与销毁 | `add_spawnable_scene(scene)`、`spawn(scene = null, pre_spawn = Callable()) -> Node`、`despawn(node)`。零参选择首个模板。主客端成功调用时当场拿到本地节点，业务无需 `await`。可选预回调执行一次。同一调用栈内给返回的 Shared 节点赋值，观察端在生成信号中读取最新字段。断线、拒绝、重发及迟到确认均有回收路径。 |
| 权威与 RPC | `has_authority()`、`want_authority(want, send_interval = 0)`，支持请求者被批准或拒绝并能观察结果。分别记录取得本地节点、权威变化和第一次有效写入。显式映射枚举，555 的 PlayerAttached/Dynamic 是 1/2，Core 中是 2/1。对象 RPC 对齐 `rpc_to(target, callable, ...)`、结果对象和失败回调；验证发给 owner 的请求落到正确写者。插件专用目标在提供处理端前明示不支持。 |
| 用户兴趣键 | 业务在本端调用 `Netcode.add_user_key(key)` 和 `remove_user_key(key)`。房主按发起连接验证玩家身份并更新该玩家的 User 订阅。按 `interest_destroy_on_exit` 分别处理保留与释放，区分离开视野和世界对象销毁；重新进入时恢复最新基线。 |

## 实施顺序与验收门槛

| 任务 | 依赖、改动位置 | 本阶段通过条件 |
| --- | --- | --- |
| T0 [已通过] 固定对照用例 | Godot 4.6.1 与原版 555，Cloud Asia 记录了逐端有序事件和实例默认值。 | [T0](验收/T0.md) |
| T1 [已通过] 原生节点和编辑器 | 扩充 ClassDB、生成配置与编辑器入口，旧全局类型改用兼容名称。 | 干净工程验证挂脚本、共享资源、属性选择、容量、撤销重做及重读。[T1](验收/T1.md) |
| T2 [已通过] IP 房间与单例 | 原生会话处理节点统一推进，房间属性在 Core 合并后广播。 | 三进程验证初值、客端更新、晚加入。检查非法地址、占用、连不上和同端口重开。[T2](验收/T2.md) |
| T3 [已通过] 客端生成与首次复制 | 模板身份和字段布局分开。首次对外发布前采集 Shared 字段，关联暂存节点与正式对象。 | B 当次取得三种模板。A 的第一个字段可包含返回后赋值，时序与 555 Web 相符。异步拒绝和单程注入 35 ms 的取消及离房均回收。[T3](验收/T3.md) |
| T4 [已通过] 权威、RPC 与兴趣 | 现有 Core 权威用于批准和拒绝。RPC 回执来自执行端。用户键按发起连接订阅。 | 最终包通过写权交接、Owner RPC、失败回调、重入和隐藏销毁。[T4](验收/T4.md) |
| T5 [已通过] 迁移 sync-lab 与发布 | 三模板共享 `orb.gd` 和配置，原生生成器替代预建池。 | 最终 ZIP 通过 128、512、原生导出和旧工程回归。原版对照在 Cloud Asia 通过，Local 服务未监听。[T5](验收/T5.md) |
| T6 [已通过] 空间 AOI、服务端输入 | 原生区域节点订阅网格。ServerReplicator 复用 Core 输入历史。 | 编辑器运行和导出 Player 均通过空间重入及带序号重演，队列最终清空。[T6](验收/T6.md) |

## 验收与迁移

- **负载。** 两端核对 `version`、`energy`、`active`、`label`、`offset`、`trail`、`tags`、`sent_at`。128 档按“全量 → 键 1 → 键 2 → 全量”应得到 **128 → 76 → 76 → 128**，逐项核对对象编号、实例释放和重建。512 档单组应为 **268**。同时记录 10 Hz 目标负载下的版本推进、固定步率、状态年龄 P95 和 TCP 队列，跟改造前同机同配置的 Netcode 基线比较。
- **旧工程。** 在替换注册类型前列出 `scene_replicator.gd`、`scene_spawner.gd`、`replicated_object.gd`、`scene_room.gd` 的路径、UID、继承和预加载调用方。为旧场景提供明确的转换映射，保留已有导出值、资源引用及业务脚本；对依赖旧异步生成 API 的调用方单独迁移。先在副本验证 yard、puzzle、shiftline，再更新其 SDK 依赖。若新增 Core 消息会改变协议，明确版本识别与旧端行为，验证不同版本在协商时给出清晰结果。
- **证据。** ClassDB 和实例默认值、双端有序事件、负载与导出复跑结果，分别记录在本任务的 `验收/`。现有 DemoCheck 用于负载回归；首次字段、真实释放、写权和信号顺序由独立用例断言。Fusion Local 对照使用已运行的 Photon Local 服务，Netcode 直连只需房主和客端。

在 `netcode/` 执行 `python -B tools/package_godot_addon.py` 构建候选包。在 `examples/sync-lab/` 将 `NETCODE_BENCH_SDK` 指向新 ZIP 的绝对路径，再运行：

```powershell
.\launch.bat -Editor
.\launch.bat -Backend Netcode -Objects 128 -DemoCheck -Trace
.\launch.bat -Backend Netcode -Objects 512 -DemoCheck -Trace
.\launch.bat -Backend Fusion -Objects 128 -DemoCheck -Trace
```

在 `examples/` 使用 `python -B tools/verify_sdk_install.py godot` 验证独立 SDK。验收报告记录断言、退出码和构建标识；双窗成功启动只说明启动链路正常。

## 实现入口

原生节点和编辑器适配在 Godot SDK，协议记录和会话版本在 Core。接入、旧场景迁移与复跑命令见[接入规格](../../docs/specs/godot-scene-sync.md)和[T5 验收](验收/T5.md)。

[原版接口清单](../../research/fusion/findings/godot-types.tsv) · [Godot 适配层](../../engineplatform/godot/) · [Core 生成请求](../../core/session/world.cpp) · [现有接入规格](../../docs/specs/godot-scene-sync.md) · [Fusion 后端样例](../../../examples/sync-lab/backend_fusion.gd) · [Netcode 后端样例](../../../examples/sync-lab/backend_netcode.gd)

## v5 复审后的用法差异

- Shared 动态对象的同栈赋值与首个生成信号已对齐。Fusion 的常驻地图注册、网络场景加载和继承父权威的子对象生成使用独立入口。[生成验收](验收/T3.md#对齐返回后赋值) · [Fusion 生成手册](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/spawning)

- 预测仍由 Netcode 房间处理器自动推进。Fusion 555 还提供玩法显式推进 `process_input_queue(delta)`、队列累计时长查询和重演前 `state_reset`。Netcode 目前用 `is_new` 标记各次重演。[预测证据](验收/T6.md#v5-输入消费)
- 客端 Owner RPC 需要房主核验写权代次，确认后在接收端执行。Netcode 原生 RPC 走可靠控制通道。[T4](验收/T4.md#原生协议-v4-的-rpc-和写权)
- Fusion Godot 官方所有权文档允许 Transaction 持有人主动转为无主，后续玩家直接认领。当前 Netcode 客端释放后由房主接管，后续普通请求继续审批。原版 555 的主动释放尚未单独跑双端轨迹。[Fusion Transaction](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/replication/ownership-modes#transaction)
