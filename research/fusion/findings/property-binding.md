# property-binding · 已有证据与待证问题

适用版本见[构建指纹](../builds/racing-3.0.0.md)。以下静态结论来自工作区已有的[逐地址分析记录](../../../../position-replicator-example/Racing属性同步实现.md)。两条可独立运行的基线见[复现步骤](../probes/README.md)。

## 会影响数据结构的结论

| 项目 | 记录 |
| --- | --- |
| 主张 | 在指定 Linux 构建的普通字段缓存项中，目标节点指针、实例 ID、属性名和 NodePath 一同存在。 |
| 静态依据 | 缓存项内 `+0x00`、`+0x08` 分别写入指针和 ID；`0x19d0ab` 向 `+0x10` 构造属性名，`0x19d0bc` 向 `+0x18` 调用 NodePath 的复制构造。普通出站路径 `0x19dd6a` 使用节点和属性名读值，`0x19dd93` 按 word 偏移计算写入位置。 |
| 替代解释与区分方法 | “仅配置保存路径”也能解释一次成功同步，乃至一次 `Object::get` 调用。向 **slot 本身** 复制构造 NodePath 的指令能区分这两种布局。 |
| 边界 | 槽中存在路径已定位。其全部用途、重绑分支具体使用哪一份路径以及每物理帧实际调用 getter 的次数仍需各自证据。上述偏移限于这一份 Linux 库。 |
| 实现影响 | Godot 参考模型可表示绑定路径和缓存目标。跨引擎 Core 的字段布局按[本项目决定](../../../docs/decisions/engine-boundary.md)维护。 |

## 可复跑的两种观测

- **原版 Windows 插件**：同一个刚体启用 AUTO，顺次添加三个标量后，`compute_word_count` 返回 `15, 16, 17, 18`。这测量该配置的布局用量；字段具体偏移、线上包长度和采集频率需另外验证。
- **仓库外教学实现**：手动推进采集后，模拟丢弃 tick 2、tick 3 重传、B 应用、ACK 后 tick 4 为空。它可作自研设计的回归参照。Fusion 的实际重传与 getter 时序尚无这个场景下的双端运行记录。

## Core 中的 Shadow 与确认布局

对同一 Linux 构建，`CheckForMutatedState` 在 `0x45a840` 和 `0x45a850` 读取对象偏移 `+0x1e8` 和 `+0x1f8` 对应的数组指针，以 4 字节为步长比较两组值。变化时 `0x45a871` 覆盖第二组的对应值，`0x45a885` 更新对象 `+0xc8` 指向的逐 word 记录。原版公开对象说明将第一、第二组分别称为 Words 和 Shadow。

`WriteObjectRoot` 先扫描，再在 `0x462db1` 读取对象 `+0xc0` 的值作为 `WriteDirtyWords` 的阈值。`PacketDelivered` 在 `0x4593ec` 写入相同偏移。两条路径分开，说明逐 word 计算的变化可以在 Shadow 已更新后仍被选中发送。逐 word 上报规则有静态指令和公开契约支持。此构建下双端收到确认的具体时序仍需要原版在线探针。[Fusion Core 对象布局](https://doc.photonengine.com/fusion-core/v3/manual/objects#shadow-buffer) · [调用点与复查命令](../../../docs/architecture.md#原版调用与指令证据)

## 下一条可区分的观测

同时记录原版 A 的脚本写入、复制属性 getter 次数、物理帧号以及 B 的实际应用。增加关闭该字段复制的控制组。若 getter 持续发生于没有业务读取的测量窗口，可进一步改变采集驱动与发送设置，检验“每物理步读一次”和“仅在特定出站条件下读取”的适用范围。
