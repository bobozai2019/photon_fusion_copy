# property-binding · 已有证据与待证问题

适用版本见[构建指纹](../builds/racing-3.0.0.md)。以下静态结论来自工作区已有的[逐地址分析记录](../../../../references/experiments/position-replicator-example/Racing属性同步实现.md)。两条可独立运行的基线见[复现步骤](../probes/README.md)。

## 会影响数据结构的结论

| 项目 | 记录 |
| --- | --- |
| 主张 | 在指定 Linux 构建的普通字段缓存项中，目标节点指针、实例 ID、属性名和 NodePath 一同存在。 |
| 静态依据 | 缓存项内 `+0x00`、`+0x08` 分别写入指针和 ID；`0x19d0ab` 向 `+0x10` 构造属性名，`0x19d0bc` 向 `+0x18` 调用 NodePath 的复制构造。普通出站路径 `0x19dd6a` 使用节点和属性名读值，`0x19dd93` 按 word 偏移计算写入位置。 |
| 替代解释与区分方法 | “仅配置保存路径”也能解释一次成功同步，乃至一次 `Object::get` 调用。向 **slot 本身** 复制构造 NodePath 的指令能区分这两种布局。 |
| 边界 | Linux 出站路径从 `0x19dc78` 起以实例 ID 回查指针对象，失效后从 `0x19dc9d` 转入重建。原版 Web 双端的同路径替换及稳定计数见下文。具体偏移限于这一份 Linux 库。 |
| 实现影响 | Godot 参考模型可表示绑定路径和缓存目标。跨引擎 Core 的字段布局按[本项目决定](../../../docs/decisions/engine-boundary.md)维护。 |

## 离线、在线和教学实现的观测

- **原版 Windows 插件**：同一个刚体启用 AUTO，顺次添加三个标量后，`compute_word_count` 返回 `15, 16, 17, 18`。这测量该配置的布局用量；字段具体偏移、线上包长度和采集频率需另外验证。
- **原版离线 getter 对照**中，五次显式布局计算对一个已配置字段调用 getter 五次，移除该字段时为零。保留配置并等待 30 个未入房的物理帧也为零。在线计数要扣除布局阶段的调用。[字段、容器与根节点实测](native-runtime.md)
- **原版 Web 双端**在 60 和 30 Hz 下分别稳定运行 60 个物理步。A 的已配置字段各读 60 次，相同脚本关闭该字段后读 0 次，B 的稳定字段读写均为 0。A 将旧子节点移出树但保留实例、在同路径挂新节点，B 先读到旧实例的 11，释放旧实例后才读到新实例的 22。单独调用布局计算时会立即找到新路径目标。[条件与完整结果](client-audit.md)
- **仓库外教学实现**手动推进采集，模拟丢弃 tick 2、tick 3 重传、B 应用、ACK 后 tick 4 为空。该特定丢包顺序在原版云房间中尚未受控注入。

## Core 中的 Shadow 与确认布局

同一脚本回调连续写 `0.125、0.375、0.875` 的实测中，权威端三次赋值同属一个物理帧，代理的 setter 全窗口只收到一次约 `0.88`。改变变量三次、采到一个当前值、解码后存在量化误差可以同时发生。[原版单帧测试与浮点对照](client-audit.md)

对同一 Linux 构建，`CheckForMutatedState` 在 `0x45a840` 和 `0x45a850` 读取对象偏移 `+0x1e8` 和 `+0x1f8` 对应的数组指针，以 4 字节为步长比较两组值。变化时 `0x45a871` 覆盖第二组的对应值，`0x45a885` 更新对象 `+0xc8` 指向的逐 word 记录。原版公开对象说明将第一、第二组分别称为 Words 和 Shadow。

`WriteObjectRoot` 先扫描，再在 `0x462db1` 读取对象 `+0xc0` 的值作为 `WriteDirtyWords` 的阈值。`PacketDelivered` 在 `0x4593ec` 写入相同偏移。两条路径分开，说明逐 word 计算的变化可以在 Shadow 已更新后仍被选中发送。逐 word 上报规则有静态指令和公开契约支持。此构建下双端收到确认的具体时序仍需要原版在线探针。[Fusion Core 对象布局](https://doc.photonengine.com/fusion-core/v3/manual/objects#shadow-buffer) · [调用点与复查命令](#原版调用与指令证据)

### 接收更新标记与普通字段回写

| 项目 | 核对结果 |
| --- | --- |
| 主张 | Preview 555 的 Godot 接收路径消费对象级更新标记。普通直接字段从本地 Words 解码，并和上次解码的 Variant 比较，变化时才调用属性 setter。 |
| 静态依据 | 指定 Linux 编辑器库的 `ReadObjectData` 在 `0x45dd07` 把对象 `+0x41` 写为 1。`HasBeenUpdatedByPlugin` 在 `0x457b00` 读取该字节，reset 为真时清零。Godot 调用点 `0x19fb75` 传 reset=true，返回非零时进 `0x19f92e` 读取对象 `+0x1e8` 的 Words 指针。普通字段分支在 `0x19fae5` 一带按 word 偏移解码，`0x19fb18` 和字段缓存 `+0x28` 的 Variant 比较，不同时在 `0x19fb2b` 更新缓存，经 `0x19fb5b` 调 Godot `Object::set` 包装。 |
| 边界 | 地址限于上述指纹的 Linux Preview 555。Web 稳定普通属性测得 getter 和 setter 各 0 次。官方 Godot 文档另列出插值、强制重置及 `Array[T]` 的行为。AOI 销毁重建和 Transaction 写权交接实验覆盖可见结果，未对这些转换逐字段计数。 |
| 可区分的解释 | 仅凭稳定帧无 setter，也可能是每步读取节点后做本地值比较。原版普通字段分支比较的是缓存 Variant，且 Web 实验中代理本地改值在邻字段更新后仍保留，两项证据区分了这两种路径。 |
| 实现影响 | Netcode 的可选路径复用 Core 快照和对象版本，在物理步里检查版本，然后比较已应用的网络字段值。Netcode 的 AOI 重绑和写权基线按本地契约验证。 |

[原版本地改值实验](property-profile.md#行为对照与实现边界) · [官方 Core 接收示例](https://doc.photonengine.com/fusion-core/v3/integration/object-sync-patterns) · [官方 Godot 字段与重置](https://doc.photonengine.com/fusion-godot/v3-shared-authority/manual/replication/syncing-properties)

### 八人场剩余的房间推进开销

| 项目 | 核对结果 |
|---|---|
| 主张 | 原版权威客户端先问状态通道能否入队，再从对象的 word 变更记录直接写差量。Photon Cloud 按接收方的兴趣键分发，重新进入兴趣组时补完整状态。 |
| 依据 | 同一 Linux 编辑器库中，`PacketQueueState` 在 `0x4633d4` 调用 `Notify::Connection::CanQueue`。返回假时走提前返回分支，为真时在正常状态路径从 `0x4635c7` 调用 `WriteObjectRoot`。`WriteDirtyWords` 的 `0x45a950` 筛选超过 ACK 阈值的变化，在 `0x45a980 / 0x45a982` 写 word 位置差，在 `0x45a996 / 0x45a9a1` 对值执行 ZigZag 与变长写入。原版调用点脚本复核 35/35 通过。[调用点](#原版调用与指令证据) · [序列化](https://doc.photonengine.com/fusion-core/v3/manual/serialization) · [AOI](https://doc.photonengine.com/fusion-core/v3/manual/interest-area) |
| 边界 | 反汇编核对的是 Preview 555 随包客户端。云房间的候选索引、压缩和排队策略缺少内部证据。公开架构表明本地提交与云端逐接收方分发分工明确。[Fusion 拓扑文档](https://doc.photonengine.com/fusion-godot/v3-client-server/getting-started/choose-topology) |
| 实现影响 | 自研离线可用房主在本地执行逐客人复制。本机分段计时显示，512 对象写入期间的快照压缩远重于筛选和打包，先降低现有压缩器查找成本。若可见覆盖率下降或房主发送排队增加，再分别验证差量编码与发送预算。 |
| 替代解释与区分方法 | 低帧率也可能由 AOI 对象扫描、传输轮询或引擎帧结果构造引起。分段记录各自耗时可区分这几个阶段。判定 Cloud 负载则需云端遥测或同拓扑对照。 |

## 原版调用与指令证据

下列地址为 Racing 3.0.0 随包 Linux x86-64 编辑器库的 ELF 虚拟地址。间接符号调用通过 `.plt.sec` 的跳转槽及重定位表核对。Godot 绑定内部缺少可用的函数名时使用地址。

| 路径 | 已核对的实际调用或指令 |
| --- | --- |
| Godot 绑定 | `0x12b568 → 0x1a6cd0`，再从 `0x1a6cdc` 调用 `0x19db30`。普通字段在 `0x19dd6a` 调用 Godot 的 getter 包装，`0x19dd93` 计算 `base + 4 × wordOffset` |
| 发送与分类 | `UpdateFrameEnd` 的 `0x464193` 调用 `PacketQueue`。后者在 `0x463f48 / 0x463f50 / 0x463f59` 分别调用可靠 RPC、不可靠 RPC 和状态的排队函数。`PacketQueueState` 在 `0x4635c7` 调用 `WriteObjectRoot` |
| Shadow 与差量 | `WriteObjectRoot` 在 `0x462dac` 调用 `CheckForMutatedState`。`0x45a860–0x45a885` 从两组等步长 32 位数组读取并比较，不同时回填旧值数组并更新第三组数组的变化 tick。随后 `0x462dbf` 调用 `WriteDirtyWords`，后者在 `0x45a950` 比较逐 word 变更版本和传入阈值。`0x462db1` 读取的对象确认字段也由 `PacketDelivered` 在 `0x4593ec` 更新 |
| Notify 与传输 | 状态在 `0x463a9a` 进入 `Notify::Connection::Queue`。`Send` 在 `0x4899d6` 调用 `SendChannel`，后者在 `0x48991b` 调用 `SendFragment`。分片经平台虚函数交付，Photon 平台的 `Send` 在 `0x455f70` 调用 `PhotonMatchmaking::RealtimeClient::SendEvent` |
| 接收和生灭 | `PhotonNotifyPlatform::Recv` 在 `0x468708` 分派到 `StatePacketReceived`。状态解析在 `0x46569d` 调用 `ReadObjectData`，在 `0x465917` 尝试通知对象就绪，在 `0x465c63` 可处理远端对象销毁 |

这些证据可以区分两种差量实现。一种把当前值与上次已确认的完整快照比较，另一种把当前值与上次扫描值比较、再通过独立的变更记录和确认阈值重发。原版的两处分离调用及确认字段写入支持后者。SDK 客户端的证据无法确定云房间内部的存储布局。从框架仓根目录可用同一份库复查。下列命令分别列出符号、PLT 跳槽与 Shadow 扫描指令，其他调用点可用同样的地址范围查看。[构建指纹](../builds/racing-3.0.0.md) · [Fusion Notify 通道](https://doc.photonengine.com/fusion-core/v3/reference/notify-api)

```powershell
$sdk = '../references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion/bin'
$so = "$sdk/libfusion.linux.editor.x86_64.release.so"
llvm-nm -D -n -C $so | Select-String 'CheckForMutatedState|WriteDirtyWords|PacketQueueState'
llvm-objdump -d -j .plt.sec --start-address=0xd48c0 --stop-address=0xd48d0 $so
llvm-objdump -R --demangle $so | Select-String 'CheckForMutatedState'
llvm-objdump -d --start-address=0x45a830 --stop-address=0x45a8a0 $so
```

## 下一条可区分的观测

在物理频率固定时改变发送设置，再分别观察 getter、远端应用和传输计数，可研究采集与打包调度的关系。既有测量的物理频率、对照组和采样窗口均可复跑。[在线探针](../probes/cloud_web.py)
