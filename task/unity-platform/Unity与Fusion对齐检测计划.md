# Unity Netcode 与 Photon Fusion 对齐检测计划

目标是让 Unity 开发者保留一段 Fusion 玩法的结构、能用相同的 Prefab 组织对象，并在同样的输入和网络条件下看到相同的游戏行为。保留 Netcode 命名。产品验收使用 Unity 对 Unity。首轮建立差异清单，每一项写明证据、影响、归属和复测方式，再决定改动。

## 对照版本与当前起点

| 对象 | 已核对的事实 | 对检测的影响 |
| --- | --- | --- |
| Photon Fusion for Unity | 建议固定 **2.1.4 Stable Build 2500**。官方下载页列出的 Unity 6 分支有 6.0.x、6.3.x。本项目包声明 6000.4，历史 Player 使用 6000.4.0f1。 | 先用同一 6000.4.0f1 导入两个 SDK、构建同配置 Player。若原版兼容性有差异，记录导入结果，并在双方均能运行的相同 Editor 补做对照。跨 Editor 数据单列。 [官方下载与版本要求](https://doc.photonengine.com/fusion/v2/getting-started/sdk-download) · [本项目包声明](../../engineplatform/unity/package.json) |
| Netcode Core | 已有定长 32 位 word 布局、发送扫描用的 shadow、每个 word 的变化版本、按连接维护的 ACK 和完整基线。工作区另有按变化接收字段的 `ScriptGame::receive_replica`，Unity 通用桥当前调用 `sync_replica`。 | 验证这些路径在两个 Unity Player 里何时命中，确认上下行和旧包协商。Godot 的 1024 对象结果作为用例来源。 [WordState](../../core/state/value.hpp) · [连接进度](../../core/state/replication.hpp) · [两种本地同步入口](../../core/session/script_game.cpp) · [Unity 桥](../../engineplatform/unity/game.cpp) |
| Unity 通用 SDK | 普通成员在绑定时解析反射信息。每次有效的 `NetcodeReplica.FixedUpdate` 读取所有绑定成员，组装 `object[]`。托管层编码后调用原生桥，编码会逐字段查询 schema。返回需要应用的状态时，再解码并遍历返回字段写入。玩法样例在 `Update` 调用 `Advance()`。 | 即使网络包很小，普通属性在主机和代理上也可能产生采样、装箱、跨边界调用和写回成本。实际占比、静止代理的纠正语义、刚体路径均需单独测。 [组件](../../engineplatform/unity/managed/NetcodeReplica.cs) · [编解码](../../engineplatform/unity/managed/NetcodeGame.cs) · [通用玩法样例](../../../examples/yard/unity/Assets/GameplayComponents.cs) |
| 实际交付 | 规划时实读的 Unity 开发安装为 `929c4f1663907b23/d155c48545cdd908`。按示例锁解析的 UPM tgz 内为 `7c86097d5ebf426f/d155c48545cdd908`。车场主场景另走 `NetcodeYard → YardReplicator`。 | 每份结果记录两个 Player 实际报告的 Core/Unity build ID、UPM 哈希和入口。分开列开发安装、候选发行包、当前锁包与车场回归。 [SDK 验收记录](../../docs/sdk-validation.md) · [示例包锁](../../../examples/sdk-lock.json) · [车场入口](../../../examples/yard/unity/Assets/YardScene.cs) |

Fusion Unity 的公开契约包括由 IL Weaver 将网络属性的 getter/setter 接到对象的状态内存，以及可独立读取的模拟状态、快照和变化探测器。检测分别记录**游戏属性访问、发送差量、游戏变化通知**三层。“与上次发送扫描的值比较”“与某个接收者的确认基线比较”“两次调用检测器之间发生了哪些变化”各有自己的时间基准。Fusion Unity 的私有 shadow 数量、地址布局、包头和 ACK 粒度暂列未知。原 Godot Preview 555 的结论保留原平台与版本标签。[网络属性](https://doc.photonengine.com/fusion/v2/manual/data-transfer/networked-properties) · [快照与变化检测](https://doc.photonengine.com/fusion/v2/manual/advanced/network-buffers)

## 检测范围

P0 用最小可玩的网络 Prefab 和真实双端验证主流程。P1 逐项盘点进阶能力，标注支持、部分支持、缺失或待实测，并给出影响的玩法。P2 用于独立扩展专项。命名可映射，新增配置、手工注册、回调时机变化和可观察副作用都计作差异。

为防止漏项，G0 从指定构建导出公开的组件、接口、属性标记、回调、配置项和编辑器入口，与 Netcode 逐项映射。对每一项记录对应入口、调用者、默认设置、可用拓扑、支持状态和用例编号。下表给出检测重点；在原版清单里发现的额外能力也需登记。不同命名只记录一次对应关系。

| 优先级 | 检测项与责任层 | 必做场景和判据 |
| --- | --- | --- |
| P0 | **接入、组件与烘焙** · Unity 平台 | 在空工程用一个 Runner、一个球 Prefab、两个网络 Behaviour 和一项网络属性启动双端。逐步对照挂组件、声明属性、Prefab 注册、烘焙、生成、读写和清理。记录 Netcode 多出的配置表、路径、ID、字段顺序、手工绑定和每帧调用。覆盖嵌套网络对象、场景对象、Prefab 变体、脚本重编译、重复或不支持的字段，以及编辑器错误能否定位到成员。 [Fusion 网络对象](https://doc.photonengine.com/fusion/v2/manual/network-object) · [当前导出器](../../engineplatform/unity/managed/NetcodeLayoutExporter.cs) |
| P0 | **Runner 与模拟时序** · Unity 平台，必要时追到 Core 调度 | 记录入房、输入采集、网络 tick、前向模拟、重演、快照提交、Render、退出的逐 tick 顺序。强制出现一帧 0、1、多个 tick。Host/Client 验证输入归属、校正和重演；Shared 独立验证持权端推进及代理显示；Single 用于离线对照。记录当前手动泵和各组件 FixedUpdate 的相对顺序。 [Fusion Runner](https://doc.photonengine.com/fusion/v2/manual/network-runner) · [模拟与渲染循环](https://doc.photonengine.com/fusion/v2/concepts-and-patterns/fusion-update-loop) |
| P0 | **网络属性与内存** · Unity 平台 + Core | 为同一对象列出 Behaviour、属性类型、起始 word、宽度、容量、默认值、状态有效期和读写者。覆盖标量、向量、旋转、结构体、枚举、固定容量字符串/集合、对象/Behaviour 引用。测集合单项修改、缩短、超容量、失效引用、对象池复用。画出两套 SDK 从属性访问到状态及快照的内存图，标明已知布局、逻辑视图、实际复制和未知项。 |
| P0 | **差量、上下行与恢复** · Core，Unity 验证真实入口 | 分别跑房主写→客机代理、客机持权写→房主→第二客机。覆盖全量首包、长期不变、同 tick 多写、两次发送之间 A→B→A、一个向量只改一分量、两客进度不同、丢包/乱序、迟加入、AOI 重入、量化切换、授权先于基线、旧包字段回退。固定状态向量对照恢复结果。验证 Fusion 默认同一对象属性同 tick 可见的契约；另记可选 Eventual 模式。交付上下行流程图，区别对象状态字节、压缩后包和实际链路字节。 [Fusion 对象一致性](https://doc.photonengine.com/fusion/v2/manual/data-transfer/eventual-consistency) · [Core 复制规格](../../docs/specs/property-binding.md) |
| P0 | **代理接收、变化通知与显示** · Unity 平台 | 分别观察首次 Spawned 读值、后续渲染变化通知、模拟态前后值和快照 From/To 加 alpha 插值。用同帧多写、跳过中间值、无变化、代理擅自改显示值、回调内禁用/销毁对象区分行为。记录普通属性 getter、setter、变化回调和 Transform 应用次数；标明模拟 tick 与显示帧。 [Fusion 变化回调](https://doc.photonengine.com/fusion/v2/manual/data-transfer/change-detection) · [Fusion 快照视图](https://doc.photonengine.com/fusion/v2/manual/advanced/network-buffers) |
| P0 | **对象寿命、权威与输入** · Unity 平台 + Core | 对照 Spawn 前初始化、Spawned、首次远端可读、Despawn、父子对象、场景切换及重用。把本地/远端、State Authority、Input Authority、Shared Master 分列。测试非持权写、抢占/释放、持权端掉线、授权中旧包迟到、输入缺失及重演副作用。每种拓扑记录谁能请求、谁裁决、何时开始可写、其他玩家首个可见值。 [Fusion 对象与权威](https://doc.photonengine.com/fusion/v2/manual/network-object) |
| P0 | **RPC、兴趣与发送频率** · 平台 API + Core | 从玩法声明调用 RPC，逐项核对来源/目标、本地回调、发送者、可靠/不可靠、tick 对齐、晚加入和对象消失；用持久属性验证晚加入状态。AOI 用两订阅者验证进出、重入首值与每对象优先级。在 Fusion 2.1，Host/Server 为接收者设置优先级，Shared 由接收者为自己设置。记录本项目的实际控制方。 [Fusion RPC](https://doc.photonengine.com/fusion/v2/manual/data-transfer/rpcs) · [2.1 发送优先级](https://doc.photonengine.com/fusion/v2/getting-started/release-notes/whats-new-2-1) |
| P0 | **Transform、性能与分发** · Unity 平台 + 示例/工具 | 用普通网络属性和 NetworkTransform 等价组件各跑一遍。测位置、旋转、缩放选项、父级切换、Teleport、插值首尾。用下文负载拆出主客成本。空工程安装候选 tgz，重建 Windows x64 Player，验证实际加载的 DLL 和构建标识。 [Fusion NetworkTransform](https://doc.photonengine.com/fusion/v2/manual/network-transform) · [净装验证入口](../../../examples/tools/verify_sdk_install.py) |
| P1 | **拓扑、场景和运维行为** · 平台 + Core/服务边界 | 建能力矩阵，逐列查 Dedicated Server、同进程 Multi-Peer、场景管理与叠加加载、对象提供器/池、房间列表和匹配、重连/身份恢复、Host Migration、Shared Master 切换、统计和网络模拟器。记录各自依赖、替代工作流、失败条件。优先复用现有迁移和场景探针。 [Fusion 场景管理](https://doc.photonengine.com/fusion/v2/manual/scene-loading) |
| P1 | **物理、高级数据与构建** · Unity 平台 + Core | 列出 2D/3D 刚体、休眠/唤醒、碰撞、预测/重演、Forecast Physics 与可选 Physics Addon 的适用模式。另查延迟补偿/Hitbox、服务端历史查询、大数据 RPC/可靠数据流、高级集合与类型、自定义插值、Inspector/统计工具、Mono 与 IL2CPP/AOT、裁剪和干净导入。对缺口给出独立重现和优先级。 [2.1 版本功能](https://doc.photonengine.com/fusion/v2/getting-started/release-notes/whats-new-2-1) · [2.1 物理扩展](https://doc.photonengine.com/fusion/v2/addons/physics-addon-2.1) |

P2 专项可覆盖移动端/Web、其他传输提供商和大房间容量。品牌与 API 拼写、Fusion ↔ Netcode 互连、逐字节协议兼容、Unity ↔ Godot/Cocos 混合房间、复刻 Photon Cloud 服务端内部实现均在本计划范围外。Photon Cloud 对 Unity 开发者可观察的入房、断线和 Shared 权威行为仍归上表检测。

## 先跑哪几个实验

1. **一个球，移植同一段玩法。** 在 Fusion 和 Netcode 各建一个等价 Prefab，声明血量、颜色、位置和持权交互。用相同业务输入完成生成、修改、显示、一次 RPC 和销毁。交付可并排阅读的玩法脚本和 Inspector 配置表，把仅需改名的调用、额外样板代码、语义变化逐项圈出。
2. **一个对象，两个进度不同的接收者。** 同 tick 修改两个 Behaviour 的相关字段，用对象内序号校验首次可读与字段组合，再让一个接收者丢包、离开 AOI 后返回。反向跑一次客机持权。逐段记录源状态、发送扫描、连接基线、收到快照、模拟可读、渲染回调和最终值；Fusion 内部不可观测阶段留空。这个实验能区分“链路最终收敛”和“玩法在中间读到了半个对象”。
3. **1024 个球，定位成本。** 复用 Godot sync-lab 的工作负载描述，在 Unity 建等价场景。建议从 1、128、512、1024 个对象逐级跑，每对象 8 字段，60 Hz 模拟、10 Hz 业务写入起测，发送频率单独记录并匹配。测静止、稀疏单字段变化、全量运动、AOI 切换、换权。裸 Unity 无同步、现版 Netcode、原版 Fusion、后续候选版各用同样的 Prefab/渲染和随机种子；固定窗口、VSync、帧率上限、物理设置、网络条件。分开做无窗口纯同步和双窗口体验，交换角色重复至少三次。Fusion Shared 云链路与 Netcode 本机直连分别报 RTT、传输和字节观测点，只对可比条件给出归因。

   | 样本 | 分开记录的指标 |
   | --- | --- |
   | 房主、持权客机、纯代理客机 | CPU 主线程 ms/帧和 ms/tick、帧间隔 P50/P95/P99、实际 tick/s；房主分别记录自己的对象和转发对象 |
   | 发送路径 | 业务写/采样、装箱与 GC B/帧、P/Invoke 次数、托管与原生复制字节、word 扫描、组包、包数和 B/s |
   | 接收路径 | 收包与解码、基线合并、变化字段数、代理读取/写回次数、变化回调、插值、Transform/刚体应用与渲染耗时 |
   | 算法和采集误差 | 每对象状态/历史内存、稳态分配和峰值；Profiler 开/关各留基线。附对象实际活动数、可见数与最后 tick，防止漏同步产生虚高 FPS |

先测原版和现版的重复波动，再按角色和负载冻结可接受的毫秒、字节与分配预算。行为验收要求权限、回调阶段、对象内一致性和故障恢复满足各自模式的契约。性能验收给出相对原版差距及相对旧版变化，并逐项解释超预算开销。

## 执行顺序与证据

| 阶段 | 依赖 | 小目标与通过条件 |
| --- | --- | --- |
| G0 固定基线 | 起点 | 确认原版包哈希、Netcode 两个构建标识、Unity 版本、角色与网络设置。两套 SDK 的最小 Player 走各自真实入口，能输出相同格式的事件轨迹。 |
| G1 提取契约 | G0 | 完成 Prefab 移植、生命周期轨迹、属性内存图和上下行流程图。接口表登记参数类型、默认值、有效阶段、调用角色、失败结果及证据。单个事件至少标明角色、对象、Behaviour、字段、frame、tick、阶段和可见值。 |
| G2 功能与成本 | G1 | 同时开展弱网/交权实验、P1 能力盘点和热路径计数。性能计时独占测试机。每项判为一致、确认差异、项目扩展、模式不适用或待证；差异要能指出首个分歧阶段。 |
| G3 确定改造边界 | G2 | 每个确认差异记录玩法影响、复现、预期、替代解释与区分实验。属性访问、组件生命周期、PlayerLoop、Interop 先归 Unity；基线、对象一致性和协议进度在共用层复现后归 Core；Yard 专用桥和负载场景归示例。逐项排改动顺序。 |
| G4 两轮复核 | G3，后续实施时 | 第一轮用原版、现版、候选版重跑已确认差异；第二轮从候选 UPM 干净安装，以另一角色/网络种子重跑 P0、既有游戏回归和 1024 性能。实际 Player 的包标识可追到被测产物。 |

规划状态 **[已完成]**。检查点为本项目入口、两个构建来源与原版公开契约。执行状态 **[未开始]**，G0 至 G4 的双端实验、判定和修复分别在执行时登记。原版联机会用到 Fusion App ID；若当时无法使用云房间，先完成离线生命周期、烘焙、布局与本项目双端实验，云侧项目保留待证。

首个执行动作是建立同 Editor、同输入的单球场景。原版版本证据归 `research/fusion/builds/`，能力结论归 `research/fusion/findings/`，差异矩阵与两轮运行报告归本任务的 `验收/`。已有入口如下，以下命令从 **examples 仓库根目录**运行。第一条默认验证锁包；可用 `--archive <候选tgz>` 验证新包。通用玩法和车场回归需先以目标 SDK 构建对应 Player。新增的 Fusion 双端对照与 Unity 1024 负载工具在执行阶段补齐。

```powershell
python -B tools/verify_sdk_install.py unity
python -B tools/build.py --unity --sdk-root ../netcode/out/install/win-x64-msvc-mt
python -B tools/verify_gameplay_components.py --engine unity
python -B tools/verify_yard.py play --platform unity --port 43264 --fault
```
