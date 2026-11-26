# Godot 场景中的 RPC 与属性同步

面向同一 Godot 游戏的多个客户端。房主和客人使用相同的场景、脚本与字段声明。房主协调对象身份、写权与消息转发，普通玩家可以成为某个对象的状态写者。[三人示例](../../../examples/puzzle/godot/sync/scene_controls.gd) · [场景配置](../../../examples/puzzle/godot/sync/scene_controls.tscn)

## 原生节点接入

1. 在运行场景添加 `NetcodeSpawner`，将双方使用的 `PackedScene` 按相同顺序放入 `spawnable_scenes`。同一房间可以有多个生成器，双方按相同顺序装入场景树。
2. 在模板根节点下添加 `NetcodeSharedReplicator`。默认 `root_path=".."`，根复制为 None，权威模式为 Transaction。网络节点允许直接挂业务脚本，如 `extends NetcodeSharedReplicator`。
3. 为 Sync 选择一个 `NetcodeReplicationConfig`。在底部 **Netcode 同步属性** 面板选择根或子节点属性，设置容量和插值并保存资源。多个模板可以引用同一个 `.tres`。启用 Auto 后会加入对应根节点的运动字段。
4. 房主调用 `Netcode.create_room("127.0.0.1", 43231, options)`，客端调用 `Netcode.join_room("127.0.0.1", 43231)`。`OK` 表示已受理。使用 `room_joined`、`connection_failed(reason)` 和 `room_left` 维护连接状态。可以在 `room_joined` 中调用 `spawn()`。读取仿真时钟前查看 `Netcode.is_simulation_ready()`，游戏开局可用房间属性单独控制。

```gdscript
@onready var spawner: NetcodeSpawner = $Pool/Spawner

func spawn_orb(initial_score: int) -> Node:
    var orb := spawner.spawn()
    if orb:
        orb.set("score", initial_score)
    return orb
```

成功时当次返回本地节点。可选预回调执行一次，顺序为预回调、入树、`_ready`、Replicator 的 `spawned()`、Spawner 的 `spawned(node)`、调用返回。需要让本地 `_ready` 或本地生成信号读取初值时，用 `spawn(scene, func(sync, node): ...)`。

Shared 动态对象在第一次对外发布前采集已配置的字段。在同一回调中直接给 `spawn()` 返回的节点赋值，观察端的第一个复制基线和两个生成信号会读到更新后的值。观察端的 `_ready` 仍先读取场景默认值。客端握手就绪后按序提交生成，待发送和待确认的实例合计上限为 64。需要正式网络身份或写权时查看 `is_write_ready()`，给复制的 Node 属性赋值时目标须已完成网络绑定。ServerReplicator 需要在预生成回调设置预测模型使用的初始字段。

校验失败返回 `null`。客端请求被拒绝或等候超时后触发 `spawn_failed(node, reason)` 并释放。离房回收仍待确认的实例。房主或当前写者可以调用 `despawn(node)`，生成者也可以取消尚待确认的本地实例。回调取消生成或移除实例时，后续生成信号停止。[原版单字段对照](../../task/godot-platform/验收/T0.md)

`Netcode.get_room().get_players()` 返回已完成原生协议与模板协商的 `NetcodePlayer` 记录，按 `number` 排序。`room_joined` 回调里可以读取初始名单，之后通过 `Netcode.player_joined(player_id, user_id)` 和 `Netcode.player_left(player_id, is_inactive)` 接收变化。客端在首次 `room_joined` 回调返回后才公布启用的仿真时钟，房间属性与初始状态同帧到达时也遵循这一顺序。TCP 房间最多 64 席，包含房主。TCP 直连用数字玩家 ID，`user_id` 和 `name` 为空，离开的玩家立即释放席位。

房主可在 `NetcodeRoomOptions.custom_properties` 设置初值。`set_property` 与 `set_properties` 提交键值补丁，`remove_properties(keys)` 删除键，`set_properties_cas(patch, expected)` 仅在预期的现有属性全部匹配时提交整批修改。客端方法返回请求是否已发出，`properties_changed` 提供已提交的完整属性。房主调用 CAS 直接返回匹配及提交结果。每房最多 64 个键，每值编码最多 4 KiB，整个基线最多 32 KiB。

`has_authority()` 报告本地写权，`is_write_ready()` 表示正式写权的基线已应用。房主和客端都用 `want_authority(true)` 请求 Transaction 写权。当前写端可在 `authority_requested(requester)` 的同步回调中返回 `bool`。有投票时，任一回调返回 `true` 即批准。玩法也可用 `answer_authority(requester, approved)` 显式应答，先提交的显式应答优先。请求方收到 `authority_response(approved)`，再等待写入基线。房主需要立即执行管理员收权时使用 `reclaim_authority()`。正数 `send_interval` 以 Core tick 为单位控制采样间隔。

对只同步根节点普通字段的 `NetcodeSharedReplicator`，可在 `NetcodeReplicationConfig` 开启 `apply_proxy_changes_only`。绑定时读取此设置，`is_proxy_changes_only_enabled()` 返回实例实际绑定的模式。代理收到新状态时只写入变更字段。代理上的本地改值保留到该字段的网络值变化或重新应用完整基线。代理首次挂载、AOI 重绑和写权代次切换仍应用完整基线。此选项默认为关闭。在包含自动运动、插值、子节点路径、对象引用、普通 Array 或 Dictionary 的配置中，采用逐步采样和本地纠偏。

用 `Netcode.rpc(orb.apply_score, 3)` 向所有能看到对象的玩家发送，`Netcode.rpc_to(Netcode.TARGET_OWNER, orb.apply_score, 3)` 发给当前写端，`Netcode.rpc_to_player(player_id, orb.apply_score, 3)` 发给特定玩家。可调用对象为 Sync 或其复制根，方法用 `@rpc` 标注，已有的 `rpc_methods` 和 `sync.rpc_to()` 也可继续使用。同一写权代次的 Owner 调用最多等待接收端基线三秒，过期代次会失败。插件目标返回明确错误。结果通过 `is_finished()`、`get_error()` 和 `on_fail(callback, ttl)` 读取。失败回调可用零参 `func()` 或一参 `func(reason: String)`。

房间广播的脚本节点在入树时调用 `Netcode.register_broadcast_receiver(self)`，出树时调用 `Netcode.unregister_broadcast_receiver(self)`。广播方法用 `@rpc` 标注，对应的接收节点按方法名匹配。同名广播方法应使用一致的注解。广播可以发给全房、房主或单个玩家。`@rpc("authority")` 限制对象写端或房主发送，`call_remote` 跳过发送端。路由目标包含发送端时，`call_local` 在发送受理后、调用返回前执行一次。客端的 `TARGET_OWNER` 调用先等房主确认写权代次，Owner 基线未就绪时继续等待。未标注的旧 `rpc_methods` 沿用队列投递。原生 RPC 统一使用可靠控制通道。

处理函数可用 `Netcode.get_rpc_sender()` 读取最初发起的玩家 ID，或用 `Netcode.get_rpc_info()` 同时读取 `sender`、`target` 和 `target_node`。房间广播的 `target_node` 为 `null`。回调以外，发送者是 0，上下文是 `null`。单独的 RPC 参数可以传入已绑定的复制根。接收端按网络身份查找自己的节点，节点销毁或超出兴趣范围时收到 `null`。

在 Sync 的 `user_keys` 中声明对象兴趣键，客端通过 `Netcode.add_user_key(key)` 或 `remove_user_key(key)` 调整本端订阅。客端生成模板前须订阅至少一个匹配键。房主保有完整场景，调用本地用户键方法会返回 `false`。退出视野发 `interest_exit(node)`，`interest_destroy_on_exit=true` 会释放实例，否则暂停绑定并保留节点。重入应用最新基线，保留的节点继续使用原实例。全局销毁会释放保留实例。

## 空间兴趣与输入预测

给移动观察者添加 `NetcodeInterestArea`，设置 `target_path` 与格子半径。被观察对象的 Sync 启用 `spatial_interest`。本版使用 16 个 Godot 单位的格子和 0 或 1 格半径。2D 与 3D 分别订阅各自的网格。空间模板和用户键模板分别选择一种分发方式。客端生成空间对象时需保持对出生格的订阅，直到写权确认。出生格不可见时，暂存实例可能等到超时后回收。

复制根可以通过 Sync 的 `spatial_position_property` 提供 3D 网格坐标。填写根节点的属性名，`Vector3` 使用世界 XYZ，`Vector2` 使用世界 XZ 且 Y 为 0。属性在模板登记时须提供格子坐标范围内的有限值。留空时使用 Node2D 或 Node3D 的变换。观察者仍使用对应维度的目标节点变换。已绑定的房主写者使用根字段的变化回写路径且每物理步都采样时，可从同一次同步采样取得坐标。代理、限频或非根字段布局继续独立读取空间坐标。

房主可在 `NetcodeRoomOptions` 启用 `retain_spatial_writer_interest`，授予客端写权后，其空间对象继续按对象频率下发给该写者，即使跨出已订阅格。地图就绪、显式对象筛选和依赖可见性继续生效。写权转让、收回后按新写者和当前订阅重新判定。该设置默认为关闭。

客端合并所有区域的格子，每次最多订阅 64 个不同格子。超过上限时，`Netcode.interest_area_overflow(cells, limit)` 在本次超限开始时发出。此时保留上次订阅中仍被请求的格子，已移除区域的格子会退出。回到上限以内后会提交完整集合，暂时无法发送时会重试。

`NetcodeInterestArea.send_divisor` 默认 1，可设置为 1 至 65535。该区域内仍有兴趣的对象按房间发送步限频。两个区域覆盖同一格时取最小除数，并遵守对象自身的发送除数。持权保留适用时使用对象的发送频率。旧房主使用原空间订阅频率。

Host 在生成 `NetcodeServerReplicator` 时可在预生成回调或正式绑定后调用 `set_input_authority(player_id)`。在 Sync 设置固定长度 `neutral_input` 与 `input_model_version`。获准的端可调用 `queue_input(delta, payload)`，大于零且不超过一秒的 `delta` 以 float32 秒数记录在指令内。初次预测、权威执行与重演在 `on_process_input(tick, delta, payload, is_new)` 收到同一个值。原 `queue_input(payload)` 继续使用房间步长。

房主拥有输入权时，`queue_input()` 当场执行并提交状态。后续服务补步会抵扣这段已执行的输入时间。处理客端输入时，房主每个 16 ms 服务步累计时间。连续队首的 delta 满足后才执行，剩余时间可继续消费后面的指令。队首等待期间暂停中立模拟。空闲服务步执行中立输入，空闲时间不会积累给下一条命令。同一个服务步内执行的多条指令各有独立的模型 tick。

房间处理器自动驱动消费和重演，两者可能在 Godot 空闲帧运行。输入玩家须能看到对象才能收到预测帧。预测与重演之前会应用对应基线，业务可用 `is_new` 跳过重复的音效等外部副作用。使用 `get_confirmed_input()`、`get_input_queue_count()` 和 `get_replayed_input_count()` 查看推进。旁观者用普通复制路径应用房主状态，按字段配置插值。输入模型适用于普通复制字段与运动字段。刚体 Auto 预测继续使用现有物理适配器。

## 旧场景迁移

默认构建包含旧脚本路径和 UID。构建时指定 `--godot-native-only` 得到只含 C++ 扩展和编辑器场景资源的安装版。使用该安装版的工程须改用下表中的原生入口，并删除项目设置里指向 `res://addons/netcode/plugin.cfg` 的旧启用项。

| 脚本路径 | UID | 继承与兼容类 | 使用原生接口时的映射 |
| --- | --- | --- | --- |
| `res://addons/netcode/scene_replicator.gd` | `uid://1uqctyllulyb` | `NetcodeLegacyReplicator` → `NetcodeReplicatedObject` | 模板 Sync 改为 `NetcodeSharedReplicator`。保留 `root_path`，将 `root_mode` 赋给 `root_replication_mode`，把 `properties`、`limits`、插值路径放入 `.tres`，将 `player_attached` 映射到 Player Attached。依赖逐字段客端写权或自定义旧采样器的逻辑使用原兼容脚本。 |
| `res://addons/netcode/scene_spawner.gd` | `uid://wdxdce6f4db1` | `NetcodeLegacySpawner` → `Node` | 改为 `NetcodeSpawner`，显式复制原 `spawn_path` 和模板列表。房间入口移到 `Netcode`，把 `await spawn(scene)` 改成同步 `spawn(scene, pre_spawn)`，需要写时查看 `is_write_ready()`。 |
| `res://addons/netcode/replicated_object.gd` | `uid://dn82v61nw0vjl` | `NetcodeReplicatedObject` → `Node` | 旧自定义绑定可继续使用该脚本。 |
| `res://addons/netcode/scene_room.gd` | `uid://ov6a5y27q00u` | `NetcodeRoom` → `RefCounted` | 旧常驻场景可继续使用。动态原生模板由单例的一个处理节点推进。 |

yard 赛车的主场景引用旧 Spawner，车辆模板引用旧 Replicator。puzzle 控制台预加载旧 Room，场景引用旧 Replicator。shiftline 的对局、玩家、手榴弹、补给与箱子使用旧 Replicator 路径，对应业务脚本的类型来自同一路径。

原生入口协商会话版本和有序模板目录指纹。旧房主拒绝协议时，客端通过 `connection_failed` 报告不兼容。未完成版本和模板协商的连接会超时并报告失败。两端都使用旧脚本入口时沿用既有消息。

## 安装 Godot 插件

从框架仓库根目录运行 `python -B tools/package_godot_addon.py` 生成 Windows x64 兼容包，归档位于 `out/packages/godot-win-x64/current/`。追加 `--godot-native-only` 时发布到 `out/packages/godot-native-win-x64/current/netcode-godot-native-win-x64.zip`，适用于已改用原生节点的工程。sync-lab 随附的插件使用后一种构建，缺少 DLL 时默认从这个发布槽补装。[sync-lab 启动说明](../../../examples/sync-lab/README.md)

把所选 ZIP 内的 `addons/netcode` 放进目标工程，重开编辑器以加载 GDExtension。编辑器面板由扩展注册。在场景树中单选一个 `NetcodeReplicator`，底部会自动展开 **Netcode 同步属性**。每行显示一个字段，可以设置插值和适用的容量。已有字段的手动移除、编辑的撤销和重做均从此面板操作。已有工具脚本的原生类型是 `NetcodeForecastBody2D`、`NetcodeForecastBody3D`、`NetcodePhysicsIsland3D` 和 `NetcodeLayoutExporter`。

无界面构建和 CI 请使用仓库的 `import_project` 进行首次导入。[安装与验证记录](../../task/gameplay-components/验收/本机.md#godot-插件安装和属性选择)

## 动态生成场景

以下 API 用于 `scene_spawner.gd` 的兼容路径。

一个 `NetcodeSpawner` 驱动一个房间。将车辆、角色或道具的 `.tscn` 拖入 `spawnable_scenes`，用 `spawn_path` 指定实例的父节点。也可以在开房前调用 `add_spawnable_scene(scene)`。两端注册相同的资源，每个模板包含一个指向该场景根的 Sync。

创建或加入房间调用 `spawner.open(host, address, port)`，返回空字符串表示已开始连接。进入运行中的房间会触发 `room_joined(player_id)`。在此信号中调用 `var actor = await spawner.spawn(scene)`。有可写字段时，返回前会等到创建者取得写权并应用基线。失败返回 `null`。房主只接受已注册的模板。每端按可见网络对象创建或移除场景实例。

对由创建者驾驶的载具，在 Sync 勾选 **Player Attached** 和 **Delegate Root Motion**，将附加的客户端可写属性勾入底部面板。玩家离开后，它持有的车辆会从其他端移除。常驻场景对象仍可使用 `NetcodeRoom` 和原有场景绑定。[两种车型](../../../examples/yard/godot/racing/main.tscn) · [双进程验证](../../../examples/tools/verify_godot_racing.py)

## 声明属性

房主可调用 `room.game.reclaim(object_id)` 收回客人持有的写权。成功后，新代沿用最后已提交的字段值，旧代在途写入会被拒绝。房主等待 `replicator.is_write_ready()` 后再继续写。使用 `room.spawn(replicator, 1)` 可为单个实例启用动态写权模式，默认生成沿用模板配置。公开的 `sample_fields()` 返回独立的整数采样快照。

每个场景实例可以用一个 `NetcodeReplicator`。在底部面板点击 **添加同步属性…**，先在左侧选根节点或子节点，再在右侧搜索并双击属性。保存的路径可以是 `:score`、`Light:visible` 或 `:stats:points`。场景启动时，根据 Godot 属性类型和路径生成复制模板。业务脚本直接读写这些变量。

- **同步范围**由 `Root Path` 决定，路径从 Sync 自身算起，默认 `..` 指向父节点。要覆盖整个 `.tscn`，可以把 Sync 直接挂在场景根下面，或把位于其他分支的 Sync 的 Root Path 指向场景根。选择器会显示复制根及其子树；所有属性路径、根运动、RPC 接收对象和对象引用都以同一个复制根为准。一个网络对象共享写者，可以分别指定客户端可写字段；单个布局上限为 64 个字段，含自动字段。
- [控制台场景](../../../examples/yard/godot/sync/sync_panel.tscn) 只配置一次 `Network/Sync`。房主和玩家各自实例化它。根上的 `:score`、另一分支的 `Indicators/Lamp:visible` 和 `Mechanism/Marker:position` 都由这个 Sync 同步。
- `AUTO` 读取 Node2D/3D 的位置和旋转、CharacterBody 的速度，或使用 RigidBody2D/3D 的刚体适配器。刚体上也可以选额外的玩法字段。选择 `NONE` 可以只同步勾选的属性。
- `client_writable` 决定授权后客人可以提交哪些属性。`delegate_root_motion` 控制自动运动字段。用 `replicator.has_authority()` 查询自己是否为对象写者；需要在交接后立即写入时，用 `replicator.is_write_ready()` 等到该次授权的基线已经应用。客人用 `room.game.claim(replicator.object_id)` 请求，房主玩法收到请求后决定是否 `answer`。
- 普通属性默认直接应用，`smooth_properties` 对连续数值做指数平滑，离散值在半个平滑周期后应用。`angle_properties` 对浮点或三维欧拉角按最短弧插值。新基线直接应用。写者修改属性后按物理步采样，多次赋值可能合成一次，晚加入者读取当前值。
- 在应用基线时，复制组件按模板顺序写入全部字段。默认模式在稳定后按呈现值和当前场景值挑选需要写入的字段，并纠正代理本地改写的普通值和容器。子节点及嵌套资源按实际路径解析，目标暂时缺失时，组件等待路径恢复，再应用当前状态。
- 支持布尔、64 位整数、浮点、字符串、向量、四元数、颜色、网络根节点引用、常用 Packed 数组、内置值组成的 Array 和 Dictionary。支持已初始化的脚本对象子属性，以及 `Array[脚本类]` 的布尔、数值、向量、四元数与颜色成员。数组元素类须为本地声明的 `class_name`、继承 RefCounted 或 Resource，且能无参构造。节点引用在各端解析为本地节点，目标不可见时得到 `null`。
- `limits` 按属性路径设置容量。字符串按 UTF-8 字节数，普通 Array／Dictionary 按编码字节数，Packed 数组按元素数。默认字符串 64 字节，字节数组与普通集合 128 字节，其他 Packed 数组 8 项。业务需按最大合法值配置。超出容量的对象采样会被拒绝。

## 声明和调用 RPC

给网络对象根节点挂玩法脚本。下面的 `room` 是本局的 `NetcodeRoom` 实例。

```gdscript
@rpc("any_peer", "call_remote", "reliable")
func request_switch(enabled: bool) -> void:
    var requester := room.get_rpc_sender()
    if $Sync.has_authority() and can_use_switch(requester):
        is_on = enabled

func press_switch() -> void:
    if $Sync.has_authority():
        is_on = true
    else:
        room.rpc_to(room.TARGET_OWNER, request_switch, true)
```

| 使用方式 | 语义 |
|---|---|
| `room.rpc(method, ...)` | 发给能看到该对象的玩家。 |
| `room.rpc_to(room.TARGET_MASTER, method, ...)` | 发给房主。 |
| `room.rpc_to(room.TARGET_OWNER, method, ...)` | 发给当前对象写者，交接代次过期的调用会被丢弃。 |
| `room.rpc_to_player(id, method, ...)` | 发给指定玩家，对象调用受可见性约束。 |
| `room.register_broadcast_receiver(self)` | 注册房间级方法的接收节点。退出时调用 `unregister_broadcast_receiver(self)`。 |

使用 Godot 的 `@rpc` 标注并为参数声明类型。支持裸 `@rpc` 和省略注解选项的写法。默认 `authority` 限制对象写者或房间协调者发送，`any_peer` 允许普通玩家发送。`call_local` 在调用被本地接受后立即执行一次，`call_remote` 仅投递到远端。调用可以省略带默认值的尾部参数，发送端补齐后按完整参数签名校验并编码。RPC 不会向晚加入者补放。

回调中的 `room.get_rpc_sender()` 返回最初调用的玩家。`room.get_rpc_info()` 还提供 `sender`、原始路由 `target` 和本地的 `target_node`。房间方法的 `target_node` 为 `null`，在 RPC 回调之外 `get_rpc_info()` 返回 `null`。判断路由时使用 `room.TARGET_*` 常量。

独立参数可以传网络节点，目标消失时接收 `null`。集合参数支持嵌套内置值，也支持具名方法的类型化 Array 和 Dictionary；禁止在集合消息中传入脚本、资源或任意对象。当前每条 RPC 的业务参数总预算为 220 字节，含可变字段长度前缀；多个可变参数平分剩余预算。返回 `true` 表示已在本地接受，业务需要确认时可回发 RPC。支持默认通道下的可靠与不可靠投递。

旧 `NetcodeRoom` 的发送入口使用 `room.rpc*()`。需要结果对象和 `on_fail()` 的新场景可使用上述原生单例。Godot 原生的 `unreliable_ordered` 和非零通道在 `NetcodeRoom` 注册时会被拒绝。Godot 自带的 `method.rpc()` 和 `method.rpc_id()` 由场景树的原生 `MultiplayerAPI` 负责。[Fusion RPC 目标](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/rpcs/basics#rpctarget-enum) · [Fusion RPC 上下文与结果](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/csharp-bindings#rpc-context-and-results)

## 运行顺序

1. 建立 `var room := preload("res://addons/netcode/scene_room.gd").new()`，在房间打开前登记房间级接收节点，再调用 `room.open(root, host, address, port, capacity)`。返回空字符串表示打开成功。动态 prefab 可放进可选的 `templates` 列表，供各端预登记。
2. 各端每帧调用 `room.advance()`。房主按玩法时机 `room.game.start()`，并调用 `room.spawn(replicator)`。对于开房时已存在的场景节点，各端按相对根节点路径自动绑定，同脚本多实例各有独立的网络身份。动态实例由玩法生成，本地用 `bind_replica(replicator, id)` 接上已收到的对象。
3. 销毁会话时调用 `room.close()`。场景对象需要一致的相对节点名；不同字段布局复用同一个根脚本时，为各布局设置不同的 `layout_key`。

本项目按可观察的脚本行为对照 Fusion Godot Shared。线上协议、容量、插值实现及编辑器界面均以本项目为准。环境是局域网玩家开房。[Fusion RPC 参考](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/rpcs/basics) · [Fusion 属性同步参考](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/replication/syncing-properties)
