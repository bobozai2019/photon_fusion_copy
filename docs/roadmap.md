# 能力路线图

范围访谈已收口。实施依照[完整执行计划](../task/cross-engine-sync/执行计划.md)，最新复现命令和七个验收入口见[执行状态](../task/cross-engine-sync/执行状态.md)。

Godot 4 接入独立登记，沿用 Cocos2d-x 的 M0–M6 门槛。[Godot 接入计划](../task/godot-platform/总纲.md) · [Godot 执行状态](../task/godot-platform/执行状态.md)

传输改造已固定 SDK 源码基线，并通过 Windows x64 C ABI、独立 Core 及旧新版 Direct 双向互通。三端消费和属性性能按各自关口登记。[传输改造方案](../task/transport-providers/改造方案.md) · [执行状态与复现入口](../task/transport-providers/执行状态.md)

## 当前活动项 · property-binding

首个能力是房主 A 向同引擎客人 B 同步浮点字段 `net_input_x`。现有 Fusion Shared Authority 样例用于研究属性机制，运行原版探针时以该对象的实际权威端为 A。可直接复跑的入口是[两条离线基线命令](../research/fusion/probes/README.md)。场景输入和待验收结果见[单属性同步规格](specs/property-binding.md)。

| 维度 | 当前状态 |
| --- | --- |
| 原版证据 | 已有 Linux 原生库的槽构造、采集调用静态证据；已在 Windows 原生插件测得刚体 AUTO 布局随三个字段增加为 15、16、17、18 words。[证据记录](../research/fusion/findings/property-binding.md) |
| 自研实现 | 独立 C++ 核心和 Cocos 4 通过双窗口联机验收。状态差量在丢包、重复、乱序下收敛，房主与三个客人已对齐百对象状态。[首版运行时契约](decisions/first-edition-runtime.md) |
| 原版与自研对照 | 原版布局与自研教学回归均可复跑。原版双端浮点探针在 Photon 区域查询收到 `ApplicationNotFound`，双端 getter 与应用时序待验证。[探针入口](../research/fusion/probes/README.md#原版--双端浮点标记) |

**属性研究的下一项实验**：准备原版双端探针。房间内确认 A 拥有车辆，在观测窗口中仅写入标记值；A 的计数 getter 记录物理帧号，B 在关闭该字段插值时记录写回值和本地帧号。让业务脚本的额外读值可辨识，并开展一个关闭该字段复制的对照。分别改变渲染和物理频率；跨端用值序列对应事件，保留各自时钟。实验入口建立后把命令登记在 probes 中。

## 能力顺序

| 顺序 | 能力 | 阶段完成时能够演示什么 |
| --- | --- | --- |
| 1 | 单对象、单属性 | A 写、采集、投递及 B 应用可分开观察，离线可复现 |
| 2 | 多属性与节点寿命 | 同节点两属性、子节点属性、目标被释放后替换的行为均有依据或显式决定 |
| 3 | 首个 engineplatform | 两个 Cocos2d-x 进程在 Windows x64 读取声明式配置；可移植 core 独立构建。其他引擎后续接入 |
| 4 | 跨进程与状态收敛 | 两个进程在丢包、重复、乱序情况下达到约定的最终状态 |
| 5 | 会话与 RPC | 验证开局后拒绝新成员、已入局客人收到房主生成或销毁的对象、房主关闭或失联、当前会话 RPC；普通客人保留席位并重连原车，正常结算后回房间等待 |
| 6 | 运动和规模 | 在本机验证 2D/3D 变换及显示位置插值；目标为 4 人（含房主）、100 个复制对象，记录活跃比例与负载。跨设备实测待具备环境 |
