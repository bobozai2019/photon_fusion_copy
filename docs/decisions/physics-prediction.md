# 车场物理同步与三引擎预测设计

车场已实现房主权威结算、本车按步预测、检查点恢复与重演，以及远车按快照插值。Unity、Godot 和 Cocos2d-x 各自在同引擎房间验证共用的 C++ 车辆模型。下文的改造前记录和扩展方案保留各自适用的阶段。原生刚体与关节组的验证范围见现行会话规格。[车场执行契约](../specs/physics-prediction.md#已实现的房主预测) · [刚体组检查点](../specs/game-session.md#三维刚体组检查点)

## 本版采用的实现

Fusion 3 Core 的 Client-Host 由房主持有状态权威。指定预测玩家把输入排队，房主执行；权威状态要求纠偏时，客端在执行输入时收到预测重置，再执行尚未确认的输入。Shared 模式由持有对象的客户端写状态，房间模式在开局时选定。本项目将房主预测与可委托对象写权组合在同一玩家房间。车辆的有符号速度是本例运动模型选择保存的恢复量。[Photon 官方模式及输入回放说明](https://doc.photonengine.com/fusion-core/v3/getting-started/modes-and-topologies)

玩家房主结算输入，通用核心保留输入历史和快照缓冲，游戏提供模拟单步与恢复状态。车场以连续输入序号确认已执行的命令，以房主模拟步定位碰撞世界。客户端采用完整检查点并重演未确认输入，省去按浮点误差阈值决定是否回放的分支。有符号车速、公开车身字段、环境变更分别由车辆模型和场景定义。离散拼图直接发布获权字段，无需实现固定步。下表记录改造前发现的分歧。[执行契约](../specs/physics-prediction.md#已实现的房主预测) · [专项测试](../../../examples/tests/prediction_test.cpp)

## 改造前的分歧点

| 工作区证据 | 现有效果 | 做预测要补的内容 |
| --- | --- | --- |
| [Match::update / simulate](../../../examples/yard/shared/match.cpp) | 房主按 30 Hz 计算车速、转向、边界和门、路障的轴对齐碰撞，每次 update 最多补 5 步。信标按函数运动。客人没有模拟步 | 抽出无网络副作用的车辆单步函数，给房主与客人调用。原模型只检测车辆与障碍，双车相撞属于新增玩法 |
| [输入与驾驶状态](../../../examples/yard/shared/match.hpp)、[输入上行和权威回写的 M2 记录](../../task/cross-engine-sync/验收/M2.md) | 每 50 ms 发送最新油门和转向。房主用实际连接找到驾驶者，接受新序号，在输入缺失超过 8 个模拟步时回中。M2 客人收到的同一记录为 `sample=11, send=7` | 客端现用 `max(tick_, applied.send_tick)` 暴露进度。需要真正的模拟步和该步已处理的输入。把发包序号 7 当模拟步会从错误位置开始重演 |
| [状态复制](../../core/state/replication.cpp)、[车场配置](../../../examples/yard/shared/replicators.cfg) | 对象最后变化的 `sample_tick`、对象版本、按连接逐对象确认、差量重发和销毁已存在。值相同则 capture 提前返回。车的七个字段是五个变换分量、油门、车灯 | 静止时仍要确认模拟和输入进度。车的有符号 `Driver::speed` 影响下一步，却没有进入快照。仅恢复相同位置、朝向和油门会得到不同轨迹 |
| [Unity](../../../examples/yard/unity/Assets/YardReplicator.cs)、[Godot](../../../examples/yard/godot/yard/replicator.gd)、[Cocos](../../../examples/yard/cocos2dx/replicator.cpp) | 远端和自车在客机都从当前显示值向新目标过渡约 100 ms，到包时重新起算。Unity 和 Godot 对象桥传 revision 和属性值，省略了对象 sample_tick | 客端要能看到连续的带 tick 样本。自车显示跟随预测，纠偏另行平滑。不能用当前显示位置作下一步的碰撞起点 |
| [房主 RPC 业务](../../../examples/yard/shared/match.cpp)、[局次与续玩规格](../specs/room-rejoin.md) | 开关门、造拆路障可在处理消息时改写世界，断线保留车辆并由房主继续运动 | 同 tick 的碰撞环境必须可还原。把影响物理的变更排到明确的 tick 边界，重连以当前车辆、碰撞世界和新的输入起点恢复 |

**可以区分两种方案的实验。** 给两辆测试车相同位置、朝向和零油门，恢复时分别置有符号车速为 0 和 30 个车场单位/秒。在空地、朝向 0、无边界钳制时，按源码推导下一步分别前进 0 和 0.91 个单位。由位置差分估速的方案在刹停、撞门或刚重连时缺少这个状态；直接传车速可用该实验验证。这个数值是源码推导值。

## 从 UE 和 Fusion 取什么

| 来源与范围 | 可借鉴的机制 | 本项目选择 |
| --- | --- | --- |
| UE Networked Physics，服务端权威 Actor | Predictive Interpolation 允许本地物理变化并根据权威状态调速度。Resimulation 保存物理帧历史，在对应帧超差时恢复并补演，渲染平滑消化跳变 | 以对象交互需求选择策略。把历史比较放在同一模拟步，把表现修正放在可视子节点。[Epic Networked Physics](https://dev.epicgames.com/documentation/unreal-engine/networked-physics-overview) |
| Fusion 2.1 Unity，Client Host / Dedicated Server | 普通物体可用内置同步或 Forecast，Physics Addon 在正向和重演 tick 驱动物理场景。客户端补演次数直接影响 CPU | 按 world 调用一次步进，记录客户端在高延迟下的补演成本。车场先只重演自车及其必要依赖。[Fusion 物理模式](https://doc.photonengine.com/fusion/v2/manual/physics/physics-overview) · [Physics Addon 2.1](https://doc.photonengine.com/fusion/v2/addons/physics-addon-2.1) |
| Fusion Godot Shared 官方手册与本地 Racing 3.0.0 预览样例 | 手册描述用姿态、速度和有限时长的 Forecast 平滑远端刚体。本地场景使用 RigidBody3D 和 FusionSharedReplicator，脚本由持权端采输入，其他端也会读取同步字段驱动车辆表现 | 借鉴远端刚体同步字段和配置。样例的对象权威、当前官方手册及本项目房主拓扑分别取证。本地已测的 AUTO 字数只支持布局结论，旧构建双端预测时序仍待观测。[Photon Godot 物理复制](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/physics-replication) · [本地构建指纹](../../research/fusion/builds/racing-3.0.0.md) · [布局探针结果](../../research/fusion/findings/property-binding.md) |

本地 Preview 555 还提供了另一条独立证据。Linux 客户端库的 Client-Server 分支会保留输入、回传已消费序号并在重置后重新分派剩余输入。该证据支持设计独立的输入确认；Shared 赛车的远端画面和这条静态分支的实际执行频率需要各自验证。[本地原生客户端记录，第 2 节](../../research/fusion/findings/native-runtime.md)

## 哪些对象值得预测

| 模式 | 对象和客户体验 | 可升级的条件 |
| --- | --- | --- |
| 快照插值 | 远车、非交互道具、展示信标。按稍早的服务器时间从带 tick 的两个样本取位姿。休眠对象保持最后状态 | 看得见持续运动，但缓冲耗尽频繁时先量抖动、调播放延迟 |
| 有限外推（Forecast） | 球、飞行道具等需要减少远端拖尾的对象。按样本年龄、线速度与角速度推到目标时间。仅在已知自由落体时加重力，遇到接触、睡眠、权威传送或到达时限即停止外推 | 若要让球与本机动态刚体互推，适配器需支持物理安全点的有界速度纠偏，并验证碰撞。仅画面外推时使用无反馈的代理 |
| 输入预测与重演 | 自车使用共用运动模型，角色可注册自己的固定步控制器。客人按输入立即推进，房主返回同 tick 权威状态，有误差时重演后续输入 | 先覆盖自车对静态世界、开关门和路障，再覆盖接触相关的动态对象 |
| 原生刚体重演 | 多车互撞、推箱、拖车或关节链。回退同一隔离世界内彼此施力的刚体和约束，按 tick 重演 | 后端交付手动单步、同组恢复、接触与事件隔离及预算实测后启用。缺少能力或不同后端混连时协商共用运动模型；无法共用模型的对象用权威快照或 Forecast |

模式按对象模板、观察者角色及有效 tick 生效。房主本地玩家只跑一遍权威步。客人旁观时使用远端策略。若日后切换控制者，应同时换输入代次、发完整基线、清除旧驾驶历史。

共用模型由房间在开局前选定。若权威物体由原生 Bullet 或 PhysX 求解，能力不足的客人仍接收该物体的权威样本，以插值或有限外推呈现。它的加入不会改变同局其他玩家使用的房主求解器。

## 分层与实际调用路径

| 位置 | 建议改动 | 关键约束 |
| --- | --- | --- |
| `core/session`、`core/state` | 房主时钟估计、输入窗口与消费确认、预测历史及同 tick 对账、物理消息编码、环境变更进度。继续复用 Room、连接身份、可靠 RPC 和现有对象复制 | 核心可用内存链路及无引擎测试世界构建。协议使用普通定宽值，业务不访问平台节点 |
| 示例仓 [`yard/shared`](../../../examples/yard/shared/) | 设计目标是让房主和本机预测共用车辆步进，按状态、输入、碰撞世界和步长返回新位置、朝向、车速和碰撞结果。车灯、门权限、生成、计分保留各自的业务入口 | 车辆常数、障碍尺寸和坐标约定随模型版本协商。轻量客人也可复用相同的输入和对账路径 |
| `engineplatform/{unity,godot,cocos2dx}` | 注册物理根、显示根和宿主世界。实现绑定/解绑、输入采样、坐标换算、读写刚体、步进或接收引擎完成通知、应用显示姿态。声明 2D/3D 的实际能力 | 网络收包只入队。节点、RID、GameObject 和求解器状态在宿主规定的线程与物理安全点访问。失效句柄、场景切换和卸载先取消回调，再解绑释放 |

建议以“本轮能做哪一步”为桥接接口，避免 `advance()` 一次包办收包、追帧、发送和展示，令 C# 或 Godot 没有机会插入真实物理步。下列名称是建议的接口语义。

```text
poll(now) → 解析消息并排队，产生可执行的 tick 或恢复计划
begin_tick(t, Forward | Replay) → 锁定本步输入、世界成员和环境变更
platform → 装入状态与输入 → 共用模型单步 / 原生世界单步 / 等待引擎完成
end_tick(t, body_states, events) → 房主采集或客端写预测历史
flush(now) → 按发送周期打包；仅提交已完成的 tick
present(render_time) → 输出可视姿态，采集下一步输入独立进行
```

手动步进后端一帧可执行多个 tick，调用单位是整个 world 或隔离的预测世界。引擎自动步进后端由物理回调确认已完成的步，跨渲染帧保留待完成的计划。采用 60 Hz 原生步、30 Hz 网络模拟时，可约定每两个物理子步组成一个 tick，输入在两个子步内固定，最后一个子步完成后采集。每个 world 只有一个时间源，宿主自动步、Runner 和重演器不能重复推进同一世界。

**注册示例。** 同一辆车可在模板 1 声明二维、控制器版本、车体和显示子节点路径、本机驾驶用输入预测、旁观用快照插值。引擎工厂把 `(round, objectId, bodySlot)` 绑定到本地物理句柄和显示节点。核心据此选择模型、管理历史，平台驱动提供以下操作。这样其他游戏可替换控制器或使用多刚体模板。

| 平台接口草案 | 何时调用，返回什么 |
| --- | --- |
| `capabilities(world, dimension)`、`bind / unbind` | 注册时分别报告安全读写、手动步进、隔离模拟、成组恢复和接触事件过滤。解绑使旧句柄代次失效 |
| `capture(tick, group)`、`restore(checkpoint, group)` | 在物理边界批量读写规范化状态。缺少成员或后端能力时返回具体原因；提交前校验整组，避免部分对象已回退 |
| `step_world(tick, inputs, dt, pass)` 或 `on_step_completed(tick)` | 手动世界执行固定步；自动世界报告已完成的步。两条路径都能上报同 tick 状态和可去重的事件 |
| `present(body, pose)` | 在渲染阶段只写显示目标；对象的权威副本、预测体和显示值可分别读回 |

配置演进时要同时检查 `Layout::parse` 对车场五个模板及字段类型/数量的校验。可给物理模板另设版本化描述，并保留灯光、自定义属性的原绑定。增加车速或刚体字段时，相应调整模板校验、插件布局和打包后的配置副本。[车场模板解析](../../../examples/yard/shared/layout.cpp)

## 平台落点

### Unity 6.4

- 入口沿用 `YardScene.Update → ny_advance → Runner::update`。保留现有 ABI；用带版本和结构大小的新入口返回批量 tick 计划、权威样本及呈现姿态，C# 主线程提交该步完成结果。现有 `ObjectInfo` 的四个字段不支持原地追加。桥接按批次复用缓冲区，并配套更新 C# 结构布局测试。[原生桥](../../../examples/yard/unity-native/bridge.cpp) · [托管布局](../../../examples/yard/unity/Assets/NetcodeYard.cs)
- 共用运动模型驱动自车时，C# 只采输入和绘制。原生 3D 用 `Rigidbody`、2D 用 `Rigidbody2D`，把姿态、速度、角速度和睡眠状态在安全步读回/恢复。当前项目清单只显式列出 3D physics，使用 2D 前补齐模块并构建 Player。远端可视代理和碰撞代理各自选取时间线，`YardReplicator` 绕开由物理驱动的根。[项目依赖](../../../examples/yard/unity/Packages/manifest.json) · [现有绑定](../../../examples/yard/unity/Assets/YardReplicator.cs)
- 优先用独立场景 `PhysicsScene.Simulate(dt)` 或 `PhysicsScene2D.Simulate(dt)` 来重演；同步静态碰撞体、运动平台和预测组。使用默认场景时，明确取得对应 2D/3D 手动模拟控制权，退出时恢复原设置。验证连续补 4 步仅触发 4 次世界步、未参与物体和音效只走正常时序。[Unity 3D 手动步进](https://docs.unity3d.com/6000.4/Documentation/ScriptReference/PhysicsScene.Simulate.html) · [Unity 2D 手动步进](https://docs.unity3d.com/6000.4/Documentation/ScriptReference/PhysicsScene2D.Simulate.html)

### Godot 4.7.1，GDExtension 使用 godot-cpp 4.5

- 现为 `yard.gd::_process → bridge.advance() → Runner`，随后才读手动驾驶输入。把通用物理绑定移入 `engineplatform/godot`，样例负责车辆工厂与界面。共用模型模式在固定步锁存输入，`_process` 绘制呈现姿态；调整手动输入采集顺序以免多等一帧。节点删除/重建按 ObjectID 或有效实例重绑。[场景入口](../../../examples/yard/godot/yard/yard.gd) · [扩展桥](../../../examples/yard/godot-native/bridge.cpp)
- 原生 `RigidBody2D/3D` 路径在 `_integrate_forces(state)` 等安全点提交恢复或速度修正。该回调对应单个刚体，应建立世界级完成边界，用同一已完成步的样本汇总后再发布。首轮用带帧号的落体与碰撞探针标定读取的是步前还是步后，物理多线程开启时明确传递所有权。扩展若直接绑定物理类，需要扩充当前仅列 `RefCounted` 和 `OS` 的构建 profile。[Godot 物理状态接口](https://docs.godotengine.org/en/4.7/classes/class_rigidbody3d.html) · [扩展构建清单](../../engineplatform/godot/build_profile.json)
- 本机构建的 4.5 `extension_api.json` 中，`PhysicsServer2D/3D` 没有公开 step，`_step` 位于实现后端的 Extension 虚方法。首轮原生后端支持权威同步、远端插值和经验证的速度纠偏；需要任意次回放时使用共用模型，或另行实现可独立单步的后端。用实测的独立 world 步数、接触和恢复误差决定是否提升能力。[Godot 2D Server](https://docs.godotengine.org/en/4.7/classes/class_physicsserver2d.html) · [Godot 3D Server](https://docs.godotengine.org/en/4.7/classes/class_physicsserver3d.html)

### Cocos2d-x 4

- 现有 Windows 目标显式设置 `CC_USE_PHYSICS=0` 和 `CC_USE_3D_PHYSICS=0`。共用车场模型可以直接预测。把 `YardScene::update` 的手动输入采样放到固定步之前，`Replicator::render` 把返回的呈现姿态写到显示节点。新增原生物理采用独立构建选项，并验证相应外部依赖的 x64 链接与发布。[CMake 目标](../../engineplatform/cocos2dx/CMakeLists.txt) · [场景入口](../../../examples/yard/cocos2dx/scene.cpp)
- 原生 2D 路径使用 Chipmunk。启用物理场景后 `setAutoStep(false)`，每个固定子步调用一次 `PhysicsWorld::step(dt)`。本地 Windows 源在手动分支直接调用 `cpSpaceStep`，步进还会同步节点/刚体及延迟增删。隔离重演世界，避免画面插值改变下一步刚体输入。[Cocos 4 PhysicsWorld](https://docs.cocos2d-x.org/api-ref/cplusplus/v4x/d8/d63/classcocos2d_1_1_physics_world.html)
- 原生 3D 单独接 Bullet。相邻 v4 源的 `Scene::stepPhysicsAndNavigation` 自动调用 `Physics3DWorld::stepSimulate`，后者调用 `stepSimulation(dt, 3)`。应使用独立 world 或接管场景唯一驱动点，并显式验证 Bullet 内部子步和余时。2D 的 autoStep 开关只管 2D。两种后端各验一次睡眠唤醒、移除物体和有效步数，再开放对应模式。[Cocos 4 场景步进](https://github.com/cocos2d/cocos2d-x/blob/v4/cocos/2d/CCScene.cpp) · [Cocos 4 Bullet 包装](https://github.com/cocos2d/cocos2d-x/blob/v4/cocos/physics3d/CCPhysics3DWorld.cpp)

## 可选的客机状态权威

房间可以给某辆车选择两种写入规则。使用房主权威时，驾驶客人上报输入并预测，房主产出最终轨迹。使用客机状态权威时，驾驶客人直接模拟并提交本车的位姿、速度和控制器状态，房主把通过检查的样本提交为公开状态，再下发给其他玩家。客机无需等待往返即可确定授权范围内的运动；其他客人看到它的延迟仍包含上行、中继和播放缓冲。房间、对象创建销毁、门、路障与比赛结果继续由房主管理。Fusion Shared 提供了每个对象只有一个状态写入方的参照；房主授予并中继是本项目的协议选择。[Fusion 状态权威与拓扑](https://doc.photonengine.com/fusion/v2/manual/network-object#state-authority)

**当前代码需要改三处。**

1. Session 给每个可委托对象另存物理状态写入者、生效步和授权代次。现有 `Object.owner` 表示业务归属，RPC 用它路由，副本也会拒绝运行中更换 owner 的记录。保留该字段，租用车辆、掉线接管时只修改物理写权。[对象与 RPC](../../core/session/rpc.cpp) · [副本检查](../../core/state/replication.cpp)
2. `Match::host_message` 增加客机状态上行，按实际连接、局次、授权代次、有效步窗口和允许字段接纳。拒绝过期样本，检查有限数值、速度和位移边界，只有房主分配公开 revision。`Match::simulate` 跳过有外部写入者的车辆，保留可供查询的代理。客机收到回传的自己样本时只确认进度；拒绝、收权或授权基线变化时才按房主检查点校正。基本范围检查能拦截明显异常，无法证明整条轨迹符合玩法。竞技计分、关键碰撞可以选房主权威。[房主收包与步进](../../../examples/yard/shared/match.cpp)
3. 把“此对象在本端是否模拟、采集或呈现”的判断从全局 `isHost` 下放到对象角色。Unity 在固定步结束读取物理体或共用模型，Godot 从物理安全点提交，Cocos2d-x 从所选模型步末提交。三个适配器得到相同的授权变化通知，观察者走快照插值或 Forecast。首个样例采用共用车辆模型；允许原生刚体委托时，预先协商房主可以接管的同一模型，或明确选择失联后冻结该原生体。

**跨写入者碰撞。** 当前车场只有车对边界、门和路障，适合先按车委托。启用两车互撞或联机推箱时，将相关刚体放在同一权威模拟里。首选让这些可相互施力的对象从开局即归房主，驾驶客人保留输入预测。以后有接触组迁移需求时，再在接触前统一交权；已经意外接触则由房主用一份共同检查点裁决并向双方纠偏。两辆车各自上报对方受到的冲量会产生相互矛盾的结果。

## 实施次序与选择依据

1. **先跑通自车闭环。** 以当前 30/20 Hz、同引擎双端、静态场地为起点，拆出车辆单步、增加输入按步消费与权威检查点、恢复有符号速度。先固定门和路障位置，人工插入一次权威误差以验证重演。再加入按 tick 生效的造拆与开门。相同输入轨迹跑三个真实引擎前端。
2. **补远端播放与恢复。** 同时验证本地驾驶和远车观察视角、断线期间世界有变化、低帧率追步、丢包和乱序。快照插值作为每个引擎的基础能力。各引擎分别在自己的房间核对预测轨迹、恢复后的车速和最终权威状态。
3. **按后端打开原生能力。** Unity 先做隔离场景，Godot 先做原生采集和安全纠偏，Cocos 按 2D、3D 分别打开构建和探针。用同样的落体、撞墙、推箱、休眠及传送场景检查 2D/3D 平台绑定，达到[验收条件](../specs/physics-prediction.md)才允许声明原生重演。

另一个可行起点是直接用三套原生刚体驱动同一辆车。这适合必须复用引擎关节、悬挂或材质的玩法；接触顺序、休眠和求解器缓存会增加回放误差及接入成本。用同一输入录制，依次在空地、撞墙和双车相撞处恢复，比较共用模型与每个原生后端的同 tick 误差、纠偏频率及 p95 CPU，再决定具体车型的后端。项目现有门与路障规则使共用模型可以独立验证。
