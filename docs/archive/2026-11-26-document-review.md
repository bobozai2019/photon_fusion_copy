# 2026-11-26 项目文档核对记录

框架仓有 96 份项目文档，示例仓有 20 份。下表逐份列出核对重点。另查了两份工作区研究笔记、两份教学说明、早期方案及其模板，以及根目录的文件索引。检查以 Windows x64 本地工作树为准，第三方 SDK 随包手册和独立的 AI 开发调查 PDF 属于外部材料。

## 实际复核

| 项目 | 本轮结果 |
|---|---|
| 框架与示例 | 从工作区源码构建并安装 Debug Core，再从示例仓运行默认构建。Core 的 Debug 与已有 Release 构建各通过 9 项 CTest。车场的 Debug 与已有 Release 构建各通过 `match`、`prediction`。两种配置的拼图均通过三进程断言。Release 的 `room_probe` 实跑错房、局中拒绝、新局和房主超时。[构建入口](../../tools/build.py) · [拼图](../../../examples/tools/verify_puzzle.py) · [房间探针](../../tests/integration/room_processes.py) |
| Godot 原生 | Godot 4.7.1 在临时工程加载 3D 幸存者随附插件 `e05fe3808b391a05/3d0774b7693ff74d`。Shared 与 Simulation 各跑一个房主和一个客人，四个进程均 PASS。Simulation 两端最终确认序号均为 372、值均为 744。各示例随附的插件版本分别核对。[运行器](../../tools/verify_godot_native.py) · [本轮插件标识](../../../examples/horde-survivors-3d/addons/netcode/build-id.txt) |
| 原版 Fusion 与教学实现 | 对 Racing 解压目录、Starter ZIP 和 Preview 555 的 7z 比较 17 个同名库，逐个 SHA-256 相等，合成清单指纹与构建报告一致。Linux 编辑器库的 35 个指定调用或尾跳目标全部匹配。Windows 原版刚体配置 0–3 个附加浮点字段依次得到 15、16、17、18 words。教学扩展另行验证了三属性采样与丢包重传。[发行物复核命令](../../research/fusion/builds/racing-3.0.0.md) · [探针](../../research/fusion/probes/README.md) |
| Godot 小球同步开销 | 从随附 ZIP 中的 4 场房间记录核对单球和满载带宽、主客两端节点耗时。从另一份 ZIP 的 10 场原始计时窗口重新计算全部 5 行对象数 × 属性数的中位数区间及每球分摊。报告读数匹配。这里复算的是已有样本。[测量口径与两个附件](../../task/godot-platform/验收/sync-lab单球同步开销.md) |
| 包、命令和图 | 用示例仓实际的包查找函数验证 Core、Godot、Cocos 与 Unity 四份发行包的 SHA-256。检查文中 Python 入口及仓库归属，区分当前可用命令与历史命令。用 Chromium 渲染两仓全部 8 张 Mermaid 图，运行物理预测 HTML 的两种权威模式和迟到／准时两种输入场景。[包锁](../../../examples/sdk-lock.json) · [交互手册](../physics-prediction.html) |
| 引用与证据 | 核对本地文件、标题锚点与源码定位。检查链接的 233 份 JSON 和 3 份 JSONL 均可解析，核验性能附件 ZIP 的 CRC。属性性能主矩阵和使用修复整数候选记录的矩阵分别用仓库判定脚本重算，两次均为 15/15 通过。两仓外部链接逐页检查可访问性，并核查指向外部页面章节的锚点。源码定位按本次工作树复核，后续改动可使行号移动。 |

架构和规格描述工作区源码。已发布 SDK、开发安装、各示例随附的 DLL、原版 Fusion 和教学实现各有自己的构建与证据。跨仓相对链接以 `netcode/`、`examples/` 并排检出为前提；`out/` 及 `references/` 内的证据指向本机产物或另行取得的发行物。历史报告中的数字保留原有构建和环境。重建 11 月 22 日的旧性能矩阵还需取得报告指定 SHA-256 的候选 SDK ZIP；本轮在两仓 `out/` 中定位到了旧版 ZIP，未定位到具有该候选指纹的归档。本轮未重新进行 Unity 原版 Fusion 联机、Photon Cloud、真实公网、分机容量及完整历史性能矩阵测试。

## 示例与玩法

| 文档 | 核对重点 |
|---|---|
| [horde-survivors-3d/README.md](../../../examples/horde-survivors-3d/README.md) | 启动、怪物场景与性能回放入口，区分项目随附插件与试验用构建。 |
| [position-replicator/Racing属性同步实现.md](../../../examples/position-replicator/Racing属性同步实现.md) | 对照采样 C++ 源码、教学包和原版布局探针的输出。 |
| [position-replicator/README.md](../../../examples/position-replicator/README.md) | 按 README 运行字典示例，确认采样前后的三项属性。 |
| [puzzle/README.md](../../../examples/puzzle/README.md) | 重新构建 Debug 示例，以修正后的路径跑通三进程拼图。 |
| [README.md](../../../examples/README.md) | 核查两仓相对目录、安装前缀及四个 SDK 锁定包。 |
| [shiftline/README.md](../../../examples/shiftline/README.md) | 区分随工程运行的插件与准备脚本按锁文件安装的 SDK。 |
| [sync-lab/README.md](../../../examples/sync-lab/README.md) | 核对随附 build ID、场景和三种生成模板，以及八属性资源。 |
| [task/horde-survivors-3d/契约/战斗同步.md](../../../examples/task/horde-survivors-3d/契约/战斗同步.md) | 对照可靠事件、移动输入、消息枚举和 T3 使用的网络步长。 |
| [task/horde-survivors-3d/总纲.md](../../../examples/task/horde-survivors-3d/总纲.md) | 区分玩法目标、主客职责与已有角色、场景和资源来源。 |
| [task/horde-survivors-3d/执行状态.md](../../../examples/task/horde-survivors-3d/执行状态.md) | 对照 T1–T3 完成项、结果附件与尚待验证的分机容量。 |
| [task/horde-survivors-3d/细则/T1_双人原型.md](../../../examples/task/horde-survivors-3d/细则/T1_双人原型.md) | 双人可玩原型的验收条件与 T1 记录对应。 |
| [task/horde-survivors-3d/细则/T2_多授权.md](../../../examples/task/horde-survivors-3d/细则/T2_多授权.md) | 多人写权与交接的验收条件与 T2 记录对应。 |
| [task/horde-survivors-3d/细则/T3_六分钟与压测.md](../../../examples/task/horde-survivors-3d/细则/T3_六分钟与压测.md) | 六分钟玩法、压测条件及分机待验项与 T3 记录对应。 |
| [task/horde-survivors-3d/验收/T1.md](../../../examples/task/horde-survivors-3d/验收/T1.md) | 查阅素材来源与当次双人对局摘要。 |
| [task/horde-survivors-3d/验收/T2.md](../../../examples/task/horde-survivors-3d/验收/T2.md) | 对照委托写权、交接和两端结算的归档摘要。 |
| [task/horde-survivors-3d/验收/T3.md](../../../examples/task/horde-survivors-3d/验收/T3.md) | 核查对局、机器、网络步、样本和统计单位，区分同机与分机。 |
| [task/horde-survivors-3d/验收/验证方案.md](../../../examples/task/horde-survivors-3d/验收/验证方案.md) | 验证方案指向三份执行报告，计划条件与实测结论分别标识。 |
| [yard/godot/README.md](../../../examples/yard/godot/README.md) | 核对车场场景、玩法脚本与同引擎运行入口。 |
| [yard/shared/README.md](../../../examples/yard/shared/README.md) | 对照车场共用模板、适配器和验证工具的职责。 |
| [yard/unity/README.md](../../../examples/yard/unity/README.md) | 核对 Unity 项目、脚本及 Windows Player 的构建范围。 |

## 框架架构、规格与决策

| 文档 | 核对重点 |
|---|---|
| [AGENTS.md](../../AGENTS.md) | 核对现行目录职责、产物约定及证据分级。 |
| [docs/architecture.md](../architecture.md) | 对照对象生成、逐连接写权、快照、ACK 和 Godot 绑定入口，渲染全部架构图。 |
| [docs/archive/2026-11-24-netcode-audit.md](2026-11-24-netcode-audit.md) | 核查归档日期与 SDK 标识，保留当次发布的校验值。 |
| [docs/decisions/engine-boundary.md](../decisions/engine-boundary.md) | 核对平台读取与写回本地对象、同引擎组房的职责划分。 |
| [docs/decisions/first-edition-runtime.md](../decisions/first-edition-runtime.md) | 确认文档限定为首版 Direct 车场和字段协议。 |
| [docs/decisions/game-session.md](../decisions/game-session.md) | 按当前实现校正 GameGlobal 身份、字段类型、集合更新和发送周期源码位置。 |
| [docs/decisions/game-transport.md](../decisions/game-transport.md) | 核查传输选项、Steam 后端与首版直连的适用场景。 |
| [docs/decisions/godot-native-session.md](../decisions/godot-native-session.md) | 对照原生握手、Spawner 身份与首次属性规则；Shared 双端复跑。 |
| [docs/decisions/physics-prediction.md](../decisions/physics-prediction.md) | 核查预测与本地物理解算的区别，复跑车场 prediction 测试。 |
| [docs/decisions/transport-provider.md](../decisions/transport-provider.md) | 对照传输提供方契约和 TCP 合同夹具。 |
| [docs/decisions/unity-platform.md](../decisions/unity-platform.md) | 对照托管门面、模板与原生库的职责。 |
| [docs/fusion-core-comparison.md](../fusion-core-comparison.md) | 区分早期 Direct、逐 word 版本与后续上行合批。 |
| [docs/fusion-session-comparison.md](../fusion-session-comparison.md) | 标明对照所用 SDK，链接工作区当前的上下行构包条件。 |
| [docs/physics-prediction.html](../physics-prediction.html) | 浏览器验证两种权威模式、输入迟到和准时的四步演示。 |
| [docs/roadmap.md](../roadmap.md) | 核对同引擎验收表、活动任务与已有证据的位置。 |
| [docs/sdk-validation.md](../sdk-validation.md) | 比对已安装构建、示例随附标识和四份锁定发行包。 |
| [docs/specs/game-session.md](../specs/game-session.md) | 以 SessionWorld::spawn 核查带键与无键全局对象，区分 Godot 两种绑定入口。 |
| [docs/specs/godot-scene-sync.md](../specs/godot-scene-sync.md) | 对照原生场景资源、协议、写权和属性应用，复跑 Shared 与 Simulation。 |
| [docs/specs/object-authority.md](../specs/object-authority.md) | 对照角色、写者代次与确认职责，复跑 object_authority。 |
| [docs/specs/physics-prediction.md](../specs/physics-prediction.md) | 核查模拟和显示阶段，按匹配配置构建并运行 Debug 示例测试。 |
| [docs/specs/property-binding.md](../specs/property-binding.md) | 区分固定 word 布局、接收方 ACK 和兼容脚本导出的引用字段。 |
| [docs/specs/room-rejoin.md](../specs/room-rejoin.md) | 按车场房间状态机和世界恢复路径限定票据、离房及迁移条件。 |
| [docs/specs/rpc.md](../specs/rpc.md) | 对照 RPC 路由、方法模板和可靠投递的实现入口。 |
| [README.md](../../README.md) | 核对四种交付 SDK、TCP 传输、示例仓布局和构建命令。 |

## Fusion 研究

| 文档 | 核对重点 |
|---|---|
| [research/fusion/builds/racing-3.0.0.md](../../research/fusion/builds/racing-3.0.0.md) | 按修正的路径逐一比对三份发行物中的 17 个原生库。 |
| [research/fusion/findings/client-audit.md](../../research/fusion/findings/client-audit.md) | 区分 ClassDB、555/684 原版运行记录、公开契约及云房间观测。 |
| [research/fusion/findings/native-runtime.md](../../research/fusion/findings/native-runtime.md) | 用指定 Linux 编辑器库重新验证 35 个原生调用与尾跳位置。 |
| [research/fusion/findings/property-binding.md](../../research/fusion/findings/property-binding.md) | 复核 Preview 555 离线刚体布局，在线结论依原有记录。 |
| [research/fusion/findings/property-profile.md](../../research/fusion/findings/property-profile.md) | 将旧插件采样和回写口径与现行原生路径分开；更新失位源码引用。 |
| [research/fusion/findings/property-scale.md](../../research/fusion/findings/property-scale.md) | 对照每对象字段数、旧版字段掩码、源码入口与原统计附件。 |
| [research/fusion/findings/rigid-body-scale.md](../../research/fusion/findings/rigid-body-scale.md) | 核对可见对象、活动标记与采样口径；更新指向采样和分发的源码位置。 |
| [research/fusion/findings/sample-scripts.md](../../research/fusion/findings/sample-scripts.md) | 对照本地发行目录、ZIP 与脚本清单的相对位置。 |
| [research/fusion/probes/README.md](../../research/fusion/probes/README.md) | 实跑离线布局、教学重传和 Linux 原生探针，核对运行目录。 |
| [research/fusion/README.md](../../research/fusion/README.md) | 核查原版资料的版本、研究索引和官方手册入口。 |

## 首版与玩法任务

| 文档 | 核对重点 |
|---|---|
| [task/cross-engine-sync/总纲.md](../../task/cross-engine-sync/总纲.md) | 核对多引擎适配与同引擎验收目标。 |
| [task/cross-engine-sync/执行状态.md](../../task/cross-engine-sync/执行状态.md) | 将阶段完成情况与 M0–M6 的记录对应。 |
| [task/cross-engine-sync/执行计划.md](../../task/cross-engine-sync/执行计划.md) | 核对首版 Cocos 计划、里程碑与同引擎验收范围。 |
| [task/cross-engine-sync/验收/M0.md](../../task/cross-engine-sync/验收/M0.md) | 区分首版工具链记录与两仓现行构建入口。 |
| [task/cross-engine-sync/验收/M1.md](../../task/cross-engine-sync/验收/M1.md) | 用现行 SDK 构建树里的 room_probe 实跑拒绝、两局和超时。 |
| [task/cross-engine-sync/验收/M2.md](../../task/cross-engine-sync/验收/M2.md) | 修正 Core 与示例测试分仓后的复跑方式。 |
| [task/cross-engine-sync/验收/M3.md](../../task/cross-engine-sync/验收/M3.md) | 明确图形车场测试所在仓库，保留当次窗口记录。 |
| [task/cross-engine-sync/验收/M4.md](../../task/cross-engine-sync/验收/M4.md) | 对照闸门、路障的玩法断言和 match 测试。 |
| [task/cross-engine-sync/验收/M5.md](../../task/cross-engine-sync/验收/M5.md) | 明确车场重连脚本的当前目录，保留历史席位与对象摘要。 |
| [task/cross-engine-sync/验收/M6.md](../../task/cross-engine-sync/验收/M6.md) | 区分跨引擎诊断与同引擎验收，更正拼图和房间探针命令位置。 |
| [task/fusion-client-parity/Fusion逐项对照.md](../../task/fusion-client-parity/Fusion逐项对照.md) | 标明深化前的 13 项基线，补上后续 GameGlobal 身份规则。 |
| [task/fusion-client-parity/余项验收.md](../../task/fusion-client-parity/余项验收.md) | 对照连续迁移、物理回放的测试脚本与当次结构化记录。 |
| [task/fusion-client-parity/执行状态.md](../../task/fusion-client-parity/执行状态.md) | 核查阶段完成项与正式同引擎复跑入口。 |
| [task/fusion-client-parity/执行计划.md](../../task/fusion-client-parity/执行计划.md) | 核查对照计划中混合引擎实验的历史用途。 |
| [task/fusion-client-parity/深化状态.md](../../task/fusion-client-parity/深化状态.md) | 比对深化验收记录、Core 用例和仍有差异的能力。 |
| [task/fusion-client-parity/深化计划.md](../../task/fusion-client-parity/深化计划.md) | 对照深化计划和状态表，区分拟实现能力与完成项。 |
| [task/fusion-client-parity/验收/全流程文档审核.md](../../task/fusion-client-parity/验收/全流程文档审核.md) | 核对比较文档当次裁决所依据的会话、Godot 与世界管理源码。 |
| [task/fusion-client-parity/验收/记录.md](../../task/fusion-client-parity/验收/记录.md) | 给历史 plates 实验标明范围，并提供同引擎玩法运行器。 |
| [task/gameplay-components/执行状态.md](../../task/gameplay-components/执行状态.md) | 核对通用玩法、Godot 声明式场景的实现和记录入口。 |
| [task/gameplay-components/执行计划.md](../../task/gameplay-components/执行计划.md) | 将组件、绑定与刚体验收目标对应到本机验收记录。 |
| [task/gameplay-components/验收/本机.md](../../task/gameplay-components/验收/本机.md) | 区分两仓构建、打包和测试工具，保留当次三引擎测量。 |

## Shiftline

| 文档 | 核对重点 |
|---|---|
| [task/godot-fps/契约/联机与生命周期.md](../../task/godot-fps/契约/联机与生命周期.md) | 修复 Shiftline 对局脚本和主场景路径。 |
| [task/godot-fps/总纲.md](../../task/godot-fps/总纲.md) | 将原规划的 FPS 目录树改为已交付的 Shiftline 工程位置。 |
| [task/godot-fps/执行状态.md](../../task/godot-fps/执行状态.md) | 区分当次交付包与当前由 SDK 锁文件选择的包。 |
| [task/godot-fps/资产清单与制作计划.md](../../task/godot-fps/资产清单与制作计划.md) | 核查素材清单、源文件、样件和授权说明的位置。 |
| [task/godot-fps/验收/G1.md](../../task/godot-fps/验收/G1.md) | 核查大厅、人员表及同机多人证据入口。 |
| [task/godot-fps/验收/G2.md](../../task/godot-fps/验收/G2.md) | 对照场景、楼层与角色素材的当次验收证据。 |
| [task/godot-fps/验收/G3.md](../../task/godot-fps/验收/G3.md) | 核对房主权威对局报告与 Shiftline 对局契约。 |
| [task/godot-fps/验收/G4.md](../../task/godot-fps/验收/G4.md) | 核对开火、换弹与可破坏物体的视觉记录。 |
| [task/godot-fps/验收/G5.md](../../task/godot-fps/验收/G5.md) | 核对界面和异常恢复验收条目与测试计划。 |
| [task/godot-fps/验收/G6.md](../../task/godot-fps/验收/G6.md) | 将导出包复验与清洁解压启动限定在对应构建。 |
| [task/godot-fps/验收/资产样件.md](../../task/godot-fps/验收/资产样件.md) | 核查样件截图、资产清单及源文件。 |
| [task/godot-fps/验收计划.md](../../task/godot-fps/验收计划.md) | 改正示例脚本运行目录和锁定 SDK 的准备方式。 |

## Godot 平台

| 文档 | 核对重点 |
|---|---|
| [task/godot-platform/原生节点与Fusion用法对齐计划.md](../../task/godot-platform/原生节点与Fusion用法对齐计划.md) | 对照原生节点规划、Fusion 类型表与已有原生阶段记录。 |
| [task/godot-platform/契约/适配器.md](../../task/godot-platform/契约/适配器.md) | 标明早期车场适配器及混合诊断的适用范围。 |
| [task/godot-platform/属性同步性能优化方案.md](../../task/godot-platform/属性同步性能优化方案.md) | 核查优化目标对应的基线负载、计时方法和历史剖析附件。 |
| [task/godot-platform/总纲.md](../../task/godot-platform/总纲.md) | 更新 Godot 车场当前可玩工程的位置。 |
| [task/godot-platform/执行状态.md](../../task/godot-platform/执行状态.md) | 对照历次部署与测量，分别核查随附版、开发版、游戏专用版。 |
| [task/godot-platform/验收/sync-lab单球同步开销.md](../../task/godot-platform/验收/sync-lab单球同步开销.md) | 从 4 场带宽结果和 10 场属性矩阵复算报告区间及每球分摊。 |
| [task/godot-platform/验收/T0.md](../../task/godot-platform/验收/T0.md) | 核查 Preview 555 生成顺序探针及其构建范围。 |
| [task/godot-platform/验收/T1.md](../../task/godot-platform/验收/T1.md) | 对照原生节点与 Inspector 的原始适用环境。 |
| [task/godot-platform/验收/T2.md](../../task/godot-platform/验收/T2.md) | 核查房间单例、TCP、房间属性和协议版本的阶段记录。 |
| [task/godot-platform/验收/T3.md](../../task/godot-platform/验收/T3.md) | 核对多模板生成、首值、回收及不同协议版本的记录。 |
| [task/godot-platform/验收/T4.md](../../task/godot-platform/验收/T4.md) | 核对写权、兴趣和 RPC 的阶段结论与当前 Shared 入口。 |
| [task/godot-platform/验收/T5.md](../../task/godot-platform/验收/T5.md) | 区分首版原生包、旧工程兼容验收与当前随附包。 |
| [task/godot-platform/验收/T6.md](../../task/godot-platform/验收/T6.md) | 对照空间兴趣与输入确认的双端断言，复跑 Simulation。 |
| [task/godot-platform/验收/属性同步性能.md](../../task/godot-platform/验收/属性同步性能.md) | 核查逐场数据及各版适用范围；修正读写权、兴趣、周期和代理路径的源码定位。 |
| [task/godot-platform/验收/联机专项复测.md](../../task/godot-platform/验收/联机专项复测.md) | 核查断开与重新加入的用例边界、进程角色及日志位置。 |
| [task/godot-platform/验收/验收.md](../../task/godot-platform/验收/验收.md) | 将车场与独立 Core 探针拆成两仓复跑步骤。 |

## 权威、网络与 Unity

| 文档 | 核对重点 |
|---|---|
| [task/object-authority/执行计划.md](../../task/object-authority/执行计划.md) | 核对对象写权目标、API 和当前 Core 测试入口。 |
| [task/public-connectivity/执行状态.md](../../task/public-connectivity/执行状态.md) | 区分旧构建指纹和现行车场模板路径。 |
| [task/public-connectivity/执行计划.md](../../task/public-connectivity/执行计划.md) | 将公网部署先决条件与本机转发试验分开。 |
| [task/public-connectivity/验收/公网验收.md](../../task/public-connectivity/验收/公网验收.md) | 核对 Unity 双进程、转发入口和实际公网待验范围。 |
| [task/transport-providers/传输接口.md](../../task/transport-providers/传输接口.md) | 以公共 C 头文件核查接口版本与消息边界。 |
| [task/transport-providers/执行状态.md](../../task/transport-providers/执行状态.md) | 标明实施前的 Git 指纹，并给出 Direct 实现当前路径。 |
| [task/transport-providers/改造方案.md](../../task/transport-providers/改造方案.md) | 对照传输抽象、构建和外部接入的阶段方案。 |
| [task/transport-providers/验收/G1.md](../../task/transport-providers/验收/G1.md) | 核查 C ABI、TCP、外部夹具及固定构建记录。 |
| [task/transport-providers/验收/G2.md](../../task/transport-providers/验收/G2.md) | 核查旧 Direct 与候选版互通的构建与验证范围。 |
| [task/transport-providers/验收/G3.md](../../task/transport-providers/验收/G3.md) | 核查外部项目换传输 DLL 后的双端玩法与重连记录。 |
| [task/transport-providers/验收/G4.md](../../task/transport-providers/验收/G4.md) | 对照正式计分输入、历史队列补丁和性能矩阵边界。 |
| [task/unity-platform/Unity与Fusion对齐检测计划.md](../../task/unity-platform/Unity与Fusion对齐检测计划.md) | 区分 Unity 原版 Fusion 测试计划、本地现有证据与待测项目。 |
| [task/unity-platform/验收/验收.md](../../task/unity-platform/验收/验收.md) | 区分真实 Unity Player、同机组房与历史混合诊断。 |

## 工作区补充材料

| 文档 | 核对重点 |
|---|---|
| [工作区文件索引.html](../../../工作区文件索引.html) | 检查静态导航目标。文件数、体积和逐文件清单仅代表页面注明的 2026-11-22 快照。 |
| [Photon-Fusion-Godot-同步原理.md](../../../references/photon/Photon-Fusion-Godot-同步原理.md) | 核对所引用的 Racing 样例和原版手册，研究对象为 Photon。 |
| [Photon-Fusion-Godot-属性同步数据流.md](../../../references/photon/Photon-Fusion-Godot-属性同步数据流.md) | 核对 Godot 采集、公开缓冲区语义、接收呈现和对应的样例路径。 |
| [Racing属性同步实现.md](../../../references/experiments/position-replicator-example/Racing属性同步实现.md) | 修复搬迁后的命令路径。原版与教学版两个入口均已离线运行。 |
| [README.md](../../../references/experiments/position-replicator-example/README.md) | 核对教学场景、脚本与 SCons 入口。教学组件使用本地字典采样。 |
| [README.md](../../../archive/联机框架方案/README.md) | 保留建仓前建议，补充现行支持边界，修复两条离线复现命令。 |
| [AGENTS.md.example](../../../archive/联机框架方案/AGENTS.md.example) | 核对其为早期配套模板。现行项目约定见框架仓的 AGENTS.md。 |
