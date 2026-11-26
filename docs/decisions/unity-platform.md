# Unity 适配与框架扩展

新玩法使用 `Netcode.NetcodeGame` 访问通用会话，在 Unity 主线程推进 `Advance()`，并用 `NetcodeReplica` 绑定对象。控制台示例通过这条入口验证了成员同步、刚体与写权交接。[通用绑定](../../engineplatform/unity/managed/NetcodeGame.cs) · [控制台脚本](../../../examples/yard/unity/Assets/GameplayComponents.cs)

**Unity 有两个原生入口。** 通用玩法的 `NetcodeGame` 调用 SDK 的 `netcode_unity.dll`。独立示例仓的车场 Player 通过 `NetcodeYard.cs` 调用 `netcode_yard_unity.dll`，后者链接示例自己的 `yard_match`。下表说明车场路径。[通用插件构建](../../engineplatform/unity/CMakeLists.txt) · [车场插件构建](../../../examples/yard/unity-native/CMakeLists.txt) · [车场托管绑定](../../../examples/yard/unity/Assets/NetcodeYard.cs)

| 边界 | Unity 接入方式 |
| --- | --- |
| 传输 | 使用原 `Runner` 的后端选择。Windows 直连的地址、连接、超时和两路投递语义沿用现有实现 |
| 房间和状态 | 共用 `Match`、`AuthorityState`、`ReplicaState` 和 `Rpc`。插件提供帧视图、对象、绑定描述和车场输入指令 |
| C++ 到 C# | 玩家和对象身份使用定宽数值。帧更新在 Unity 主线程显式调用。C# 持有不透明运行时句柄，场景销毁时释放。浮点和开关转成渲染读取值，版本仍由 C++ 决定 |
| 本地绑定 | Unity 根据 schema 和对象 ID 创建节点，按相同路径解析根或子节点，分别写入位置、旋转和缩放。旋转按 Cocos 的 Z 顺时针约定生成四元数。客人的显示插值约 100 ms。车灯、闸门和信标材质在样例层处理 |
| 复用资产 | 构建脚本从示例仓的 `yard/shared/` 复制已有 `replicators.cfg` 与 `beacon.obj`。Unity 导入相同网格并用独立相机和灯光渲染 |

**第一阶段验收。** 使用两个 Unity Windows Player 运行同一份可执行联机剧本，对比对象与字段摘要、RPC 结果和恢复身份。Unity 另读回真实 `Transform` 验证 3D 轴值。产品按同引擎房间验收，已有混合引擎用例用于诊断。[构建与验收入口](../../../examples/yard/unity/README.md)

**可替换方案。** Unity 也可以重新实现房间和网络协议。那需要一套独立的编解码、重试、确认和恢复行为。当前 C ABI 独立于 Unity 类型，接入新引擎时可以保留 C++ 运行时和协议，替换宿主桥接与本地绑定。车场操作枚举和内置对象工厂归车场样例；其他玩法可在通用会话中注册自己的模板并创建对应的本地实例。现有 C ABI 的两侧与 Player 同步构建，版本兼容属于后续发布约定。

## 模板注册和对象工厂

模板注册描述对象如何上网。以信标为例，`replicators.cfg` 里的模板 `3`、版本 `1` 给两端约定了类型，并定义 `model` 下的三维变换以及根节点的 `marker`。房主与客人以同一模板解释新对象记录和后续差量。当前车场启动时由 `Layout::parse` 注册这些模板，并验证车场五种既定类型。

对象工厂负责在本地生成网络对象对应的实例。收到信标的模板号后，Unity 的 `YardScene.Spawn` 生成一个根 GameObject，导入的信标网格实例挂在 `model` 子节点。`YardReplicator` 通过传入的生成回调得到实例，再按字段路径应用位置、旋转和缩放。当前场景用 `switch` 选择五种车场对象。

使用通用会话的游戏可提供自己的模板配置，以及模板号到 Prefab 或创建回调的映射。比如添加模板 `6` 的补给箱时，先定义位置和已领取状态，再给 Unity 注册对应 Prefab。通用复制层继续管理实例 ID、对象版本、销毁和重连。Unity 车场对 Unity 车场已可直接使用现有配置和工厂。[模板注册](../../../examples/yard/shared/layout.cpp) · [Unity 本地工厂](../../../examples/yard/unity/Assets/YardScene.cs)
